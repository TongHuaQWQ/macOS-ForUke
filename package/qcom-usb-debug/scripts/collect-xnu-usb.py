#!/usr/bin/env python3
"""Collect the kernel console from a QcomXnuUsbDebug CDC-ACM device - one entry point.

This is the host side of the log channel described in
https://unixnu.com/zh/guide/debugging ("USB 调试终端") and
https://unixnu.com/zh/porting/log-capture. The device side is a CDC-ACM function
that tees the kernel console; whichever transport actually carries the bytes, this
script turns them into a raw capture plus a timestamped, de-escaped text log.

Transports, in the order the porting guide tries them:

  live CDC-ACM   /dev/ttyACM* (Linux), /dev/cu.usbmodem* (macOS), or --port
  usbmon         a pcap from `tcpdump -i usbmon1 -w x.pcap`, or the usbmon text
                 file from /sys/kernel/debug/usb/usbmon/1u (STATE.md 91d)
  panel QR       screenshots or a screen recording of the OneSend frames, decoded
                 by work/analysis/log-capture/panel_qr_decode.py (--qr-images)
  replay         any of the above already saved on disk, parsed offline (--replay)

Usage
  python3 collect-xnu-usb.py --list
  python3 collect-xnu-usb.py --output tmp/run --wait 120 --seconds 0 --no-marker
  python3 collect-xnu-usb.py --output tmp/run --port /dev/ttyACM0 --seconds 30
  python3 collect-xnu-usb.py --output tmp/run --usbmon-file uke.pcap
  python3 collect-xnu-usb.py --output tmp/run --qr-images shots/
  python3 collect-xnu-usb.py --replay saved/xnu-usb.log --output tmp/replay
  python3 collect-xnu-usb.py --self-test

Artifacts written to --output
  xnu-usb.log            the exact received bytes, appended across reconnects
  xnu-usb.decoded.log    one timestamped line per console line, escapes removed
  capture-events.jsonl   attach/detach/reconnect/stop events
  capture.json           the capture record (status, byte count, sha256, ...)
  panel.decoded.log      merged QR/OneSend text, when --qr-images is used
  usb-control.sock       optional bounded control channel (--control-socket)

Dependencies: python3 standard library only. Nothing here needs pip.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path

DEFAULT_SERIAL = 'QcomXnuUsbDebug'
DEFAULT_VID = 0x1209
DEFAULT_PID = 0x316d
MAGIC_MARKER = 'QCOM PANIC END\n'
PANEL_DECODER = Path('work/analysis/log-capture/panel_qr_decode.py')
_RESERVED = ('xnu-usb.log', 'xnu-usb.decoded.log', 'capture.json',
             'capture-progress.json', 'capture-events.jsonl')

# --------------------------------------------------------------------------------------
# The bounded control codec is optional: --replay and --self-test must work without it.
# --------------------------------------------------------------------------------------

_control = None


def control_codec():
    """Load xnu_usb_control.py next to this script, on first use only."""
    global _control
    if _control is None:
        path = Path(__file__).with_name('xnu_usb_control.py')
        if not path.is_file():
            raise RuntimeError('control codec missing: %s' % path)
        spec = importlib.util.spec_from_file_location('xnu_usb_control_host', path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        _control = module
    return _control


# --------------------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------------------

def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


def iso(moment=None):
    return (moment or utc_now()).isoformat()


def nonnegative(value):
    value = float(value)
    if value < 0 or not value < float('inf'):
        raise argparse.ArgumentTypeError('must be finite and nonnegative')
    return value


def byte_limit(value):
    value = int(value)
    if value < 0:
        raise argparse.ArgumentTypeError('must be nonnegative')
    return value


def atomic_record(path, record):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w') as out:
        json.dump(record, out, indent=2)
        out.write('\n')
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, path)


ANSI_ESCAPE = re.compile(
    rb'\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-Z\\-_]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)')


class LineSplitter:
    """Bytes in, complete console lines out, with arrival timestamps and no escapes.

    The kernel console mixes CRLF, lone CR for progress lines and ANSI colour from
    -v/debug output. A capture is only greppable if all three are normalised, so the
    raw bytes stay untouched in xnu-usb.log and this class feeds the decoded log.
    """

    def __init__(self, sink, flush_partial_seconds=2.0):
        self.sink = sink
        self.buffer = bytearray()
        self.started = time.monotonic()
        self.flush_partial_seconds = flush_partial_seconds
        self.lines = 0

    def feed(self, data):
        self.buffer.extend(data)
        while True:
            newline = self.buffer.find(b'\n')
            carriage = self.buffer.find(b'\r')
            if newline < 0 and carriage < 0:
                break
            if carriage >= 0 and (newline < 0 or carriage < newline):
                # A bare CR rewrites the current line on the console; keep it as a line.
                chunk = bytes(self.buffer[:carriage])
                del self.buffer[:carriage + 1]
                while self.buffer[:1] == b'\n':
                    del self.buffer[:1]
            else:
                chunk = bytes(self.buffer[:newline])
                del self.buffer[:newline + 1]
            self._emit(chunk)
        if len(self.buffer) > 8192:
            self._emit(bytes(self.buffer))
            self.buffer.clear()

    def _emit(self, chunk):
        text = ANSI_ESCAPE.sub(b'', chunk).replace(b'\x00', b'')
        text = text.decode('utf-8', 'replace')
        if not text.strip():
            return
        self.lines += 1
        self.sink(text.rstrip())

    def tick(self):
        """Flush a stale partial line so a hung device still yields its last text."""
        if self.buffer and time.monotonic() - self.started >= self.flush_partial_seconds:
            chunk = bytes(self.buffer)
            self.buffer.clear()
            self.started = time.monotonic()
            self._emit(chunk)

    def flush(self):
        if self.buffer:
            chunk = bytes(self.buffer)
            self.buffer.clear()
            self._emit(chunk)


class DecodedWriter:
    """Append-only timestamped text log shared by live capture, usbmon and replay."""

    def __init__(self, path, stamp=True):
        self.path = Path(path)
        self.stamp = stamp
        self.stream = self.path.open('a', buffering=1)
        self.splitter = LineSplitter(self._write)

    def _write(self, text):
        if self.stamp:
            stamp = utc_now().strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'
            self.stream.write('%s  %s\n' % (stamp, text))
        else:
            self.stream.write('%s\n' % text)

    def feed(self, data):
        self.splitter.feed(data)

    def note(self, text):
        self._write('# ' + text)

    def tick(self):
        self.splitter.tick()

    def flush(self):
        self.splitter.flush()
        self.stream.flush()
        os.fsync(self.stream.fileno())

    def close(self):
        try:
            self.flush()
        finally:
            self.stream.close()


# --------------------------------------------------------------------------------------
# Port discovery: one implementation per platform, sysfs instead of shelling out
# --------------------------------------------------------------------------------------

class PortInfo:
    def __init__(self, path, transport, **details):
        self.path = str(path)
        self.transport = transport
        self.details = details

    def as_dict(self):
        state = dict(self.details)
        state.update(path=self.path, transport=self.transport)
        return state


def _sysfs_usb_identity(tty_name):
    """Read idVendor/idProduct/serial for /dev/ttyACM0 from sysfs. Linux only."""
    identity = {}
    node = Path('/sys/class/tty') / tty_name / 'device'
    try:
        current = node.resolve()
    except OSError:
        return identity
    for _ in range(6):
        for attribute in ('idVendor', 'idProduct', 'serial', 'manufacturer', 'product'):
            candidate = current / attribute
            if candidate.is_file() and attribute not in identity:
                try:
                    identity[attribute] = candidate.read_text().strip()
                except OSError:
                    pass
        if 'idVendor' in identity:
            identity['sysfs_device'] = str(current)
            break
        current = current.parent
    return identity


def discover_ports_linux(serial, vid, pid):
    ports = []
    candidates = []
    for pattern in ('ttyACM*', 'ttyUSB*', 'ttyAMA*'):
        candidates.extend(sorted(Path('/dev').glob(pattern)))
    matched_any = False
    for device in candidates:
        identity = _sysfs_usb_identity(device.name)
        vendor = identity.get('idVendor')
        product = identity.get('idProduct')
        if vendor is not None and int(vendor, 16) != vid:
            continue
        if product is not None and int(product, 16) != pid:
            continue
        board_serial = identity.get('serial')
        if serial and board_serial is not None and board_serial != serial:
            continue
        matched_any = True
        ports.append(PortInfo(device, 'usb-cdc-acm-linux', serial=board_serial,
                              vid=vendor, pid=product,
                              product_name=identity.get('product')))
    if not matched_any:
        # No sysfs match: a ttyACM that exists is still worth offering, because a
        # serial-string mismatch must not hide the channel that actually carries logs.
        for device in sorted(Path('/dev').glob('ttyACM*')):
            identity = _sysfs_usb_identity(device.name)
            ports.append(PortInfo(device, 'usb-cdc-acm-linux-unmatched',
                                  serial=identity.get('serial'),
                                  vid=identity.get('idVendor'),
                                  pid=identity.get('idProduct'),
                                  product_name=identity.get('product')))
    return ports


def _walk_ioreg(node):
    if isinstance(node, dict):
        yield node
        for child in node.get('IORegistryEntryChildren', []):
            yield from _walk_ioreg(child)
    elif isinstance(node, list):
        for child in node:
            yield from _walk_ioreg(child)


def discover_ports_macos(serial, vid, pid):
    query = subprocess.run(['ioreg', '-a', '-l', '-r', '-c', 'IOUSBHostDevice'],
                           capture_output=True, timeout=20)
    devices = []
    if query.returncode == 0 and query.stdout.strip():
        try:
            devices = plistlib.loads(query.stdout)
        except plistlib.InvalidFileException:
            devices = []
    ports = []
    for node in _walk_ioreg(devices):
        node_vendor = node.get('idVendor')
        node_product = node.get('idProduct')
        matches_serial = serial and node.get('USB Serial Number') == serial
        matches_ids = node_vendor == vid and node_product == pid
        if not (matches_serial or matches_ids):
            continue
        callouts = sorted({child.get('IOCalloutDevice') for child in _walk_ioreg(node)
                           if str(child.get('IOCalloutDevice', '')).startswith(
                               '/dev/cu.usbmodem')})
        for callout in callouts:
            ports.append(PortInfo(callout, 'usb-cdc-acm-darwin',
                                  serial=node.get('USB Serial Number'),
                                  vid=node_vendor, pid=node_product,
                                  product_name=node.get('USB Product Name')))
    return ports


def discover_ports(serial=DEFAULT_SERIAL, vid=DEFAULT_VID, pid=DEFAULT_PID):
    if sys.platform == 'darwin':
        return discover_ports_macos(serial, vid, pid)
    return discover_ports_linux(serial, vid, pid)


def acquire_port(options, should_stop=None, events=None, budget=None):
    """Block until a usable port exists. Returns PortInfo. Raises RuntimeError on timeout.

    `budget` is the number of seconds to wait; it defaults to --wait on the first call and
    to --reconnect-window on a retry, so a fixed path such as /dev/ttyACM0 can disappear
    and come back without the capture giving up.
    """
    if budget is None:
        budget = options.wait
    if options.port:
        # A hotplugged /dev/ttyACM0 vanishes and reappears under the same name, so an
        # explicit path must be polled, not checked once.
        path = Path(options.port)
        deadline = time.monotonic() + budget
        while True:
            if path.exists():
                return PortInfo(path, 'explicit', serial=options.serial)
            if should_stop and should_stop():
                raise RuntimeError('stopped while waiting for %s' % path)
            if time.monotonic() >= deadline:
                raise RuntimeError('requested port %s did not appear within %.0f s'
                                   % (path, budget))
            time.sleep(min(1.0, options.reopen_delay or 0.2))
    deadline = time.monotonic() + budget
    last_state = None
    while time.monotonic() < deadline:
        if should_stop and should_stop():
            raise RuntimeError('stopped while waiting for the device')
        ports = discover_ports(options.serial, options.vid, options.pid)
        state = [port.as_dict() for port in ports]
        if events is not None and state != last_state:
            events('port-scan', ports=state)
            last_state = state
        usable = [port for port in ports if not port.transport.endswith('unmatched')]
        if len(usable) == 1:
            return usable[0]
        if len(usable) > 1:
            raise RuntimeError('multiple CDC interfaces match (%s); pass --port'
                               % ', '.join(port.path for port in usable))
        if ports and not usable and options.accept_unmatched:
            return ports[0]
        time.sleep(1.0)
    raise RuntimeError('no CDC-ACM port appeared within %.0f s; check dmesg for '
                       'cdc_acm, or capture usbmon and use --usbmon-file' % budget)


def open_serial(path):
    """Open a CDC-ACM tty in raw mode without discarding what is already buffered."""
    descriptor = os.open(path, os.O_RDWR | os.O_NOCTTY)
    # TCSAFLUSH would discard kernel log lines printed while IOKit/sysfs published the
    # node, which is exactly the boot output the capture exists to record.
    tty.setraw(descriptor, termios.TCSANOW)
    attributes = termios.tcgetattr(descriptor)
    attributes[2] |= termios.CLOCAL | termios.CREAD
    termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
    return descriptor


# --------------------------------------------------------------------------------------
# usbmon readers: pcap (tcpdump) and the text stream from debugfs
# --------------------------------------------------------------------------------------

PCAP_MAGIC = {0xa1b2c3d4: ('<', False), 0xd4c3b2a1: ('>', False),
              0xa1b23c4d: ('<', True), 0x4d3cb2a1: ('>', True)}
USB_LINUX = 189
USB_LINUX_MMAPPED = 220
USBMON_HEADER = 64


def usbmon_pcap_payloads(data, endpoint=None, include_control=False):
    """Yield (timestamp, bytes) for every completed IN transfer in a usbmon pcap."""
    if len(data) < 24:
        raise ValueError('pcap shorter than its global header')
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic not in PCAP_MAGIC:
        raise ValueError('not a little/big-endian pcap file')
    endian, nanosecond = PCAP_MAGIC[magic]
    linktype = struct.unpack_from(endian + 'I', data, 20)[0]
    if linktype not in (USB_LINUX, USB_LINUX_MMAPPED):
        raise ValueError('pcap linktype %d is not usbmon (189/220); did you capture '
                         'the right interface?' % linktype)
    position = 24
    while position + 16 <= len(data):
        seconds, fraction, captured, _original = struct.unpack_from(endian + 'IIII',
                                                                   data, position)
        position += 16
        packet = data[position:position + captured]
        position += captured
        if len(packet) < USBMON_HEADER:
            continue
        event = packet[8]
        xfer_type = packet[9]
        endpoint_number = packet[10]
        raw_length = struct.unpack_from(endian + 'I', packet, 32)[0]
        captured_length = struct.unpack_from(endian + 'I', packet, 36)[0]
        if event != ord('C'):
            continue
        if not endpoint_number & 0x80:
            continue                                     # OUT transfers carry no log
        if xfer_type == 2 and not include_control:
            continue
        if xfer_type not in (1, 2, 3):
            continue
        if endpoint is not None and (endpoint_number & 0x7f) != endpoint:
            continue
        payload = packet[USBMON_HEADER:USBMON_HEADER + min(captured_length, raw_length)]
        if not payload:
            continue
        timestamp = seconds + (fraction / (1e9 if nanosecond else 1e6))
        yield timestamp, payload


USBMON_TEXT = re.compile(
    r'^\s*([0-9a-fA-F]+)\s+(\d+)\s+([A-Z])\s+([A-Za-z])([io]):(\d+):(\d+):(\d+)\s+'
    r'(-?\d+):(\d+)(?:\s+\d+\s*=\s*([0-9a-fA-F ]*))?')


def usbmon_text_payloads(text, endpoint=None, include_control=False):
    """Yield (timestamp, bytes) from /sys/kernel/debug/usb/usbmon/Nu."""
    for line in text.split('\n'):
        match = USBMON_TEXT.match(line)
        if not match:
            continue
        (_tag, stamp, event, kind, direction, epnum, _device, _bus, _status, _length,
         payload) = match.groups()
        if event != 'C' or direction != 'i':
            continue
        if kind == 'C' and not include_control:
            continue
        if kind not in ('I', 'B', 'C', 'Z'):
            continue
        if endpoint is not None and int(epnum) != endpoint:
            continue
        if not payload:
            continue
        try:
            chunk = bytes.fromhex(payload.replace(' ', ''))
        except ValueError:
            continue
        if chunk:
            yield int(stamp) / 1e6, chunk


def load_usbmon(path, endpoint=None, include_control=False):
    """Return [(timestamp, bytes)] from either usbmon format, detected by content."""
    data = Path(path).read_bytes()
    if data[:4] in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4',
                    b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d'):
        return list(usbmon_pcap_payloads(data, endpoint, include_control))
    text = data.decode('utf-8', 'replace')
    if 'usbmon' in text[:200] or USBMON_TEXT.search(text):
        return list(usbmon_text_payloads(text, endpoint, include_control))
    raise ValueError('%s is neither a usbmon pcap nor a usbmon text capture' % path)


# --------------------------------------------------------------------------------------
# Panel QR / OneSend channel
# --------------------------------------------------------------------------------------

def locate_panel_decoder(explicit=None):
    """Find panel_qr_decode.py: --qr-decoder, $QUD_PANEL_DECODER, then nearby trees."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get('QUD_PANEL_DECODER'):
        candidates.append(Path(os.environ['QUD_PANEL_DECODER']))
    here = Path(__file__).resolve()
    for base in [here.parent, *here.parents, Path.cwd(), *Path.cwd().parents]:
        candidates.append(base / PANEL_DECODER)
        candidates.append(base / 'analysis/log-capture/panel_qr_decode.py')
    tried = []
    for candidate in candidates:
        tried.append(str(candidate))
        if candidate.is_file():
            return candidate, tried
    return None, tried


def decode_panel_images(images, output, explicit_decoder=None):
    """Delegate to panel_qr_decode.py. Returns (exit_code, decoded_path, message)."""
    decoder, tried = locate_panel_decoder(explicit_decoder)
    if decoder is None:
        return (2, None,
                'panel decoder not found. Pass --qr-decoder PATH or set '
                'QUD_PANEL_DECODER. Tried:\n  ' + '\n  '.join(tried[:6]))
    command = [sys.executable, str(decoder), '--input', str(images),
               '--output', str(Path(output) / 'panel')]
    result = subprocess.run(command, capture_output=True, text=True)
    decoded = Path(output) / 'panel' / 'decoded.log'
    message = (result.stderr or result.stdout).strip()
    if result.returncode != 0:
        return result.returncode, (decoded if decoded.exists() else None), message
    if decoded.exists():
        target = Path(output) / 'panel.decoded.log'
        shutil.copyfile(decoded, target)
        decoded = target
    return 0, decoded, message


# --------------------------------------------------------------------------------------
# Control bridge (unchanged semantics: bounded, private, non-blocking)
# --------------------------------------------------------------------------------------

class ControlBridge:
    """One serial reader with a bounded, nonblocking private Unix socket bridge."""
    MAX_CLIENTS = 4
    REQUEST_TIMEOUT = 5.0
    RESPONSE_TIMEOUT = 5.0

    def __init__(self, path, serial_fd, metadata_path):
        control = control_codec()
        self.control = control
        self.path = Path(path)
        self.serial_fd = serial_fd
        if os.path.lexists(self.path):
            raise FileExistsError('control socket already exists; refusing to replace it')
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.listener.bind(str(self.path))
            os.chmod(self.path, 0o600)
            self.listener.listen(self.MAX_CLIENTS)
            self.listener.setblocking(False)
        except BaseException:
            self.listener.close()
            raise
        self.clients = {}
        self.pending = None
        self.serial_out = bytearray()
        self.serial_deadline = None
        self.metadata_fd = os.open(metadata_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND
                                   | os.O_CLOEXEC, 0o600)
        self.metadata_error = None
        self.stats = dict(accepted=0, rejected=0, busy=0, forwarded=0, reply_frames=0,
                          delivered=0, timeouts=0, disconnects=0)

    def _metadata(self, event, request=None, reply=None, result=None):
        if self.metadata_fd is None:
            return
        record = dict(updated_utc=iso(), event=event)
        if request is not None:
            record.update(session=f'{request.session:016x}', seq=f'{request.seq:08x}',
                          verb=request.verb)
        if reply is not None:
            record.update(reply_status=reply.status, reply_session=f'{reply.session:016x}',
                          reply_seq=f'{reply.seq:08x}', fields=reply.fields)
        if result is not None:
            record['result'] = result
        pending = memoryview((json.dumps(record, sort_keys=True, separators=(',', ':'))
                              + '\n').encode())
        try:
            while pending:
                written = os.write(self.metadata_fd, pending)
                if not written:
                    raise OSError('control metadata write made no progress')
                pending = pending[written:]
            os.fsync(self.metadata_fd)
        except OSError as exc:
            self.metadata_error = str(exc)
            try:
                os.close(self.metadata_fd)
            except OSError:
                pass
            self.metadata_fd = None

    def _drop(self, client, result=None, reply=None):
        if client.get('closed'):
            return
        client['closed'] = True
        descriptor = client['socket'].fileno()
        if result is not None:
            self._metadata('command-result', client.get('request'), reply, result)
        if self.pending is client:
            self.pending = None
            self.serial_out.clear()
            self.serial_deadline = None
        try:
            client['socket'].close()
        except OSError:
            pass
        self.clients.pop(descriptor, None)

    def _accept(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except BlockingIOError:
                return
            if len(self.clients) >= self.MAX_CLIENTS:
                connection.close()
                self.stats['busy'] += 1
                continue
            connection.setblocking(False)
            self.clients[connection.fileno()] = dict(
                socket=connection, input=bytearray(), request=None, response=None,
                reply=None, deadline=time.monotonic() + self.REQUEST_TIMEOUT)
            self.stats['accepted'] += 1

    def read_fds(self):
        return [self.listener, *[c['socket'] for c in self.clients.values()
                                 if not c.get('closed')]]

    def write_fds(self):
        return [c['socket'] for c in self.clients.values()
                if c.get('response') is not None and not c.get('closed')]

    def wants_serial_write(self):
        return bool(self.serial_out)

    def _read_client(self, client):
        connection = client['socket']
        if client['request'] is not None or client['response'] is not None:
            try:
                extra = connection.recv(1)
            except BlockingIOError:
                return
            except OSError:
                self._drop(client, 'socket-read-error')
                return
            if extra:
                self._drop(client, 'trailing-input')
                return
            self.stats['disconnects'] += 1
            self._drop(client, 'client-disconnected')
            return
        try:
            data = connection.recv(self.control.FRAME_BYTES + 1 - len(client['input']))
        except BlockingIOError:
            return
        except OSError:
            self._drop(client, 'socket-read-error')
            return
        if not data:
            self.stats['disconnects'] += 1
            self._drop(client, 'client-disconnected')
            return
        client['input'].extend(data)
        if len(client['input']) > self.control.FRAME_BYTES:
            self.stats['rejected'] += 1
            self._drop(client, 'rejected-malformed')
            return
        newline = client['input'].find(b'\n')
        if newline < 0:
            return
        if newline != len(client['input']) - 1:
            self.stats['rejected'] += 1
            self._drop(client, 'rejected-trailing-bytes')
            return
        try:
            request = self.control.parse_request(bytes(client['input']))
        except self.control.ControlError:
            self.stats['rejected'] += 1
            self._drop(client, 'rejected-malformed')
            return
        if self.pending is not None or self.serial_out:
            self.stats['busy'] += 1
            self._metadata('command-result', request, result='busy')
            self._drop(client)
            return
        client['request'] = request
        client['deadline'] = time.monotonic() + self.REQUEST_TIMEOUT
        self.pending = client
        self.serial_out.extend(request.raw)
        self.serial_deadline = client['deadline']
        self.stats['forwarded'] += 1
        self._metadata('command', request, result='forwarded')

    def _read_ready(self, ready):
        for item in ready:
            if item is self.listener:
                self._accept()
                continue
            client = self.clients.get(item.fileno())
            if client is not None:
                self._read_client(client)

    def _flush_serial(self):
        if not self.serial_out:
            return
        if time.monotonic() >= self.serial_deadline:
            self.stats['timeouts'] += 1
            self._drop(self.pending, 'serial-write-timeout')
            return
        try:
            written = os.write(self.serial_fd, self.serial_out)
        except BlockingIOError:
            return
        except OSError:
            self._drop(self.pending, 'serial-write-error')
            return
        if not written:
            self._drop(self.pending, 'serial-write-error')
            return
        del self.serial_out[:written]
        if not self.serial_out:
            self.serial_deadline = None

    def _matches(self, request, reply):
        if reply.verb != request.verb:
            return False
        if request.verb == 'HELLO':
            return reply.seq == 0
        return reply.session == request.session and reply.seq == request.seq

    def handle_replies(self, replies):
        for reply in replies:
            self.stats['reply_frames'] += 1
            client = self.pending
            if client is None:
                continue
            request = client.get('request')
            if request is None or not self._matches(request, reply):
                continue
            client['response'] = bytearray(reply.raw)
            client['reply'] = reply
            client['deadline'] = time.monotonic() + self.RESPONSE_TIMEOUT
            self.pending = None
            self._metadata('reply', request, reply,
                           result='ok' if reply.status == 'OK' else 'error')

    def _write_client(self, client):
        response = client.get('response')
        if response is None:
            return
        try:
            written = client['socket'].send(response)
        except BlockingIOError:
            return
        except OSError:
            self._drop(client, 'socket-write-error')
            return
        if not written:
            self._drop(client, 'socket-write-error')
            return
        del response[:written]
        if not response:
            self.stats['delivered'] += 1
            self._drop(client)

    def _write_ready(self, ready):
        for item in ready:
            client = self.clients.get(item.fileno())
            if client is not None:
                self._write_client(client)

    def service(self, read_ready, write_ready, serial_writable):
        if serial_writable:
            self._flush_serial()
        self._read_ready(read_ready)
        self._write_ready(write_ready)
        now = time.monotonic()
        for client in list(self.clients.values()):
            if client.get('closed') or now < client['deadline']:
                continue
            self.stats['timeouts'] += 1
            self._drop(client, 'reply-timeout' if client.get('request') else 'socket-timeout')

    def close(self):
        for client in list(self.clients.values()):
            self._drop(client, 'capture-ended')
        try:
            self.listener.close()
        except OSError:
            pass
        self.serial_out.clear()
        self.pending = None
        if self.metadata_fd is not None:
            try:
                os.close(self.metadata_fd)
            except OSError:
                pass
            self.metadata_fd = None
        try:
            os.unlink(self.path)
        except OSError:
            pass
        return dict(path=str(self.path), metadata_error=self.metadata_error, **self.stats)


# --------------------------------------------------------------------------------------
# Live capture
# --------------------------------------------------------------------------------------

def capture_live(options, writer, events, raw_stream, state):
    """Live CDC-ACM capture with hotplug re-open. Returns the terminal status."""
    stop_requested = lambda: state['stop']
    deadline = time.monotonic() + options.seconds if options.seconds else None
    reconnect_deadline = time.monotonic() + options.reconnect_window
    bridge = None
    extractor = None
    descriptor = None
    control_path = (Path(options.output) / 'usb-control.sock'
                    if options.control_socket else None)
    status = 'duration-ended'
    first_attach = True
    try:
        while True:
            if stop_requested():
                status = 'interrupted'
                break
            try:
                info = acquire_port(options, should_stop=stop_requested, events=events,
                                    budget=(options.wait if first_attach
                                            else options.reconnect_window))
            except RuntimeError as error:
                state['error'] = str(error)
                status = 'interrupted' if stop_requested() else 'capture-failed'
                break
            state['port'] = info.path
            first_attach = False
            events('attach', port=info.path, transport=info.transport, **info.details)
            try:
                descriptor = open_serial(info.path)
            except OSError as error:
                events('open-failed', port=info.path, error=str(error))
                if time.monotonic() > reconnect_deadline:
                    state['error'] = 'open failed: %s' % error
                    status = 'capture-failed'
                    break
                time.sleep(options.reopen_delay)
                continue
            writer.note('attached to %s (%s)' % (info.path, info.transport))
            try:
                if options.control_socket:
                    os.set_blocking(descriptor, False)
                    bridge = ControlBridge(control_path, descriptor,
                                           Path(options.output) / 'usb-control.jsonl')
                    extractor = control_codec().ReplyExtractor()
                status = read_loop(options, descriptor, writer, raw_stream, bridge,
                                   extractor, deadline, stop_requested, state, events)
            finally:
                if bridge is not None:
                    state['control'] = bridge.close()
                    bridge = None
                    extractor = None
                if descriptor is not None:
                    try:
                        os.close(descriptor)
                    except OSError:
                        pass
                    descriptor = None
            if status != 'usb-disconnected':
                break
            writer.note('link down; waiting up to %.0f s for the device to return'
                        % options.reconnect_window)
            events('detach', bytes=state['bytes'])
            if not options.reopen:
                break
            reconnect_deadline = time.monotonic() + options.reconnect_window
            time.sleep(options.reopen_delay)
    finally:
        if bridge is not None:
            state['control'] = bridge.close()
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
    return status


def read_loop(options, descriptor, writer, raw_stream, bridge, extractor, deadline,
              stop_requested, state, events):
    """Read one connection until it ends. Returns a status string."""
    sha = state['sha']
    marker = b'' if options.no_marker else options.until_marker.encode()
    tail = b''
    last_sync = time.monotonic()

    def checkpoint():
        nonlocal last_sync
        raw_stream.flush()
        os.fsync(raw_stream.fileno())
        writer.flush()
        state['synced'] = state['bytes']
        last_sync = time.monotonic()
        write_progress(options, state)

    while not stop_requested():
        if deadline is not None and time.monotonic() >= deadline:
            return 'duration-ended'
        timeout = 1.0 if deadline is None else min(1.0, max(0.0, deadline - time.monotonic()))
        read_fds = [descriptor] + (bridge.read_fds() if bridge is not None else [])
        write_fds = (([descriptor] if bridge.wants_serial_write() else [])
                     + bridge.write_fds()) if bridge is not None else []
        ready_read, ready_write, _ = select.select(read_fds, write_fds, [], timeout)
        if descriptor not in ready_read:
            writer.tick()
            if state['bytes'] != state['synced'] and \
                    time.monotonic() - last_sync >= options.sync_seconds:
                checkpoint()
            if bridge is not None:
                bridge.service([fd for fd in ready_read if fd is not descriptor],
                               [fd for fd in ready_write if fd is not descriptor],
                               descriptor in ready_write)
            continue
        request = min(65536, options.max_bytes - state['bytes']) if options.max_bytes else 65536
        try:
            data = os.read(descriptor, request) if request > 0 else b''
        except BlockingIOError:
            data = None
        except OSError as error:
            events('read-error', error=str(error))
            return 'usb-disconnected'
        if data is not None and not data:
            # A CDC-ACM tty usually reports EIO on unplug and EOF when the host side
            # closes; both mean the link is down, so both may be re-opened.
            events('link-eof')
            return 'usb-disconnected'
        if data:
            # Unbuffered, bounded writes preserve raw bytes without holding the whole
            # capture in memory; partial writes are accounted for explicitly.
            pending = memoryview(data)
            while pending:
                written = raw_stream.write(pending)
                if not written:
                    raise OSError('log write made no progress')
                sha.update(pending[:written])
                state['bytes'] += written
                pending = pending[written:]
            writer.feed(data)
            combined = tail + data
            found = bool(marker and marker in combined)
            tail = combined[-(len(marker) - 1):] if len(marker) > 1 else b''
            if extractor is not None:
                replies = extractor.feed(data)
                if replies:
                    # Reply bytes reach the raw log and stable storage before any
                    # validated copy is delivered to a control client.
                    checkpoint()
                    bridge.handle_replies(replies)
                    bridge.service([], bridge.write_fds(), False)
            if time.monotonic() - last_sync >= options.sync_seconds:
                checkpoint()
            if found:
                writer.note('end marker %r seen' % marker.decode('utf-8', 'replace'))
                return 'end-marker'
            if options.max_bytes and state['bytes'] >= options.max_bytes:
                return 'capture-bound-reached'
        if bridge is not None:
            bridge.service([fd for fd in ready_read if fd is not descriptor],
                           [fd for fd in ready_write if fd is not descriptor],
                           descriptor in ready_write)
    return 'interrupted'


def write_progress(options, state):
    atomic_record(Path(options.output) / 'capture-progress.json',
                  dict(updated_utc=iso(), port=state.get('port'),
                       synced_bytes=state['synced'], sha256=state['sha'].hexdigest(),
                       status='capturing'))


# --------------------------------------------------------------------------------------
# --replay: the whole pipeline without hardware
# --------------------------------------------------------------------------------------

def classify_capture(path):
    """Say what a saved capture is: 'usbmon-pcap', 'usbmon-text' or 'raw'."""
    data = Path(path).read_bytes()
    if data[:4] in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4',
                    b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d'):
        return 'usbmon-pcap'
    head = data[:4096].decode('utf-8', 'replace')
    if 'usbmon' in head or USBMON_TEXT.search(head):
        return 'usbmon-text'
    return 'raw'


def replay(source, options):
    """Parse a saved capture offline and write the artifacts a live run would."""
    source = Path(source)
    if source.is_dir():
        for name in ('xnu-usb.log', 'raw.log', 'capture.bin'):
            candidate = source / name
            if candidate.is_file():
                source = candidate
                break
        else:
            raise SystemExit('%s has no xnu-usb.log, raw.log or capture.bin' % source)
    kind = classify_capture(source)
    output = Path(options.output)
    output.mkdir(parents=True, exist_ok=True)
    raw_path = output / 'xnu-usb.log'
    decoded_path = output / 'xnu-usb.decoded.log'
    if options.force:
        for path in (raw_path, decoded_path):
            if path.exists():
                path.unlink()
    elif raw_path.exists() or decoded_path.exists():
        raise SystemExit('replay output already exists in %s (use --force)' % output)

    chunks = []
    if kind.startswith('usbmon'):
        chunks = load_usbmon(source, options.usbmon_endpoint, options.usbmon_control)
        raw = b''.join(chunk for _stamp, chunk in chunks)
    else:
        raw = source.read_bytes()
    raw_path.write_bytes(raw)

    writer = DecodedWriter(decoded_path, stamp=not options.no_timestamp)
    try:
        writer.feed(raw)
    finally:
        writer.close()
    lines = [line for line in decoded_path.read_text().split('\n') if line]
    record = dict(status='replay', source=str(source.resolve()), source_kind=kind,
                  started_utc=iso(), finished_utc=iso(), bytes=len(raw),
                  sha256=hashlib.sha256(raw).hexdigest(), decoded_lines=len(lines),
                  decoded_log=str(decoded_path), replay=True,
                  transport='offline replay; no device was contacted')
    if kind.startswith('usbmon'):
        record['usbmon_chunks'] = len(chunks)
    atomic_record(output / 'capture.json', record)
    return record


# --------------------------------------------------------------------------------------
# --self-test: the replay path over a synthetic capture
# --------------------------------------------------------------------------------------

def self_test(verbose=True):
    """Exercise the pipeline without hardware: no /dev access, no ioreg, no usbmon."""
    import tempfile
    failures = []

    def check(condition, message):
        if condition:
            if verbose:
                print('  ok   %s' % message)
        else:
            failures.append(message)
            print('  FAIL %s' % message)
        return condition

    print('self-test 1: ANSI / CRLF / bare-CR line decoding')
    collected = []
    splitter = LineSplitter(collected.append)
    splitter.feed(b'QCOMUSB PH 19 E 0\r\n')
    splitter.feed(b'\x1b[1;32mxnu console: SET_ADDRESS\x1b[0m ok\n')
    splitter.feed(b'progress 10%\rprogress 90%\n')
    splitter.feed(b'tail without newline')
    splitter.flush()
    expected = ['QCOMUSB PH 19 E 0', 'xnu console: SET_ADDRESS ok',
                'progress 10%', 'progress 90%', 'tail without newline']
    check(collected == expected, 'five console lines decoded, escapes stripped')

    print('self-test 2: usbmon text capture')
    text_capture = (
        'ffff9b2c80acfa80 4370974441 S Ii:2:003:1 -115 0\n'
        'ffff9b2c80acfa80 4370974442 C Ii:2:003:1 0:23 23 = '
        '51434f4d55534220504820313920452030 0d0a\n'
        'ffff9b2c80acf940 4370974443 C Bo:2:003:2 0 0\n')
    chunks = list(usbmon_text_payloads(text_capture))
    check(len(chunks) == 1 and chunks[0][1].startswith(b'QCOMUSB PH 19 E 0'),
          'one completed IN transfer extracted from usbmon text')

    print('self-test 3: usbmon pcap capture')
    payload = b'QCOMUSB PH 20 E 0 CR 0\r\n'
    header = bytearray(USBMON_HEADER)
    header[8] = ord('C')
    header[9] = 1
    header[10] = 0x81
    struct.pack_into('<II', header, 32, len(payload), len(payload))
    packet = bytes(header) + payload
    global_header = struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, USB_LINUX)
    record = struct.pack('<IIII', 1, 500000, len(packet), len(packet)) + packet
    chunks = list(usbmon_pcap_payloads(global_header + record))
    check(len(chunks) == 1 and chunks[0][1] == payload,
          'one bulk-IN payload extracted from a usbmon pcap')

    print('self-test 4: replay over a synthetic capture')
    directory = Path(tempfile.mkdtemp(prefix='collect-selftest-'))
    try:
        synthetic = (b'QCOMUSB PH 19 E 0 CR 0 S 0\r\n'
                     b'\x1b[0;31mxnu console: AppleUSBXDCI attached\x1b[0m\r\n'
                     b'xnu console: endpoint 0 armed, event 0x201\n'
                     b'QCOM PANIC END\n')
        source = directory / 'synthetic.log'
        source.write_bytes(synthetic)
        options = argparse.Namespace(output=str(directory / 'out'), force=True,
                                     no_timestamp=False, usbmon_endpoint=None,
                                     usbmon_control=False)
        record = replay(source, options)
        check(record['bytes'] == len(synthetic),
              'raw bytes preserved (%d of %d)' % (record['bytes'], len(synthetic)))
        check(record['sha256'] == hashlib.sha256(synthetic).hexdigest(),
              'sha256 of the replay equals the source')
        decoded = [line for line in
                   (directory / 'out' / 'xnu-usb.decoded.log').read_text().split('\n')
                   if line]
        check(len(decoded) == 4, 'four decoded lines (%d)' % len(decoded))
        texts = [line.split('  ', 1)[1] if '  ' in line else line for line in decoded]
        check(texts[0] == 'QCOMUSB PH 19 E 0 CR 0 S 0', 'first decoded line is exact')
        check(texts[1] == 'xnu console: AppleUSBXDCI attached',
              'ANSI colour stripped from the second line')
        check(texts[2] == 'xnu console: endpoint 0 armed, event 0x201',
              'third decoded line is exact')
        check(texts[3] == 'QCOM PANIC END', 'terminal marker line is exact')
        stamps = [line.split('  ', 1)[0] for line in decoded]
        check(all(re.match(r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$', stamp)
                  for stamp in stamps), 'every line carries an ISO-8601 ms timestamp')
        check(stamps == sorted(stamps), 'timestamps are non-decreasing')
        check((directory / 'out' / 'capture.json').is_file(),
              'capture.json written by the replay')
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    print('self-test 5: the panel QR path is reachable from this entry point')
    decoder, tried = locate_panel_decoder()
    check(decoder is not None, 'panel_qr_decode.py located%s'
          % (' at %s' % decoder if decoder else ' (tried %s)' % '; '.join(tried[:3])))
    if decoder is not None:
        result = subprocess.run([sys.executable, str(decoder), '--self-test'],
                                capture_output=True, text=True)
        tail = result.stdout.strip().split('\n')[-1] if result.stdout.strip() else 'no output'
        check(result.returncode == 0, 'panel decoder self-test passes (%s)' % tail)

    print()
    if failures:
        print('SELF-TEST FAILED: %d check(s)' % len(failures))
        for failure in failures:
            print('  - %s' % failure)
        return 1
    print('SELF-TEST PASSED (all checks green)')
    return 0


# --------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path,
                        help='capture directory (required unless --list or --self-test)')
    parser.add_argument('--port', help='explicit tty device, e.g. /dev/ttyACM0')
    parser.add_argument('--serial', default=DEFAULT_SERIAL,
                        help='USB serial string to match (default %s)' % DEFAULT_SERIAL)
    parser.add_argument('--vid', type=lambda value: int(value, 0), default=DEFAULT_VID)
    parser.add_argument('--pid', type=lambda value: int(value, 0), default=DEFAULT_PID)
    parser.add_argument('--accept-unmatched', action='store_true',
                        help='use a ttyACM whose USB identity does not match')
    parser.add_argument('--list', action='store_true', help='list candidate ports and exit')
    parser.add_argument('--wait', type=nonnegative, default=120,
                        help='seconds to wait for the port to appear (default 120)')
    parser.add_argument('--seconds', type=nonnegative, default=120,
                        help='capture duration after attach; 0 runs until disconnect, '
                             'marker or signal')
    parser.add_argument('--max-bytes', type=byte_limit, default=0,
                        help='optional total byte ceiling; 0 has none')
    parser.add_argument('--sync-seconds', type=nonnegative, default=1,
                        help='checkpoint interval (default 1)')
    parser.add_argument('--stage', default='xnu', choices=['xnu', 'uefi-usb-probe'],
                        help='label for the capture record')
    parser.add_argument('--until-marker', default=MAGIC_MARKER,
                        help='stop at this byte sequence')
    parser.add_argument('--no-marker', action='store_true',
                        help='ignore markers; run to --seconds, disconnect or signal')
    parser.add_argument('--no-timestamp', action='store_true',
                        help='do not prefix decoded lines with a timestamp')
    parser.add_argument('--reopen', dest='reopen', action='store_true', default=True,
                        help='re-open the port after a hotplug (default on)')
    parser.add_argument('--no-reopen', dest='reopen', action='store_false')
    parser.add_argument('--reopen-delay', type=nonnegative, default=1.0)
    parser.add_argument('--reconnect-window', type=nonnegative, default=120,
                        help='seconds to keep looking for the device after a detach')
    parser.add_argument('--control-socket', action='store_true',
                        help='expose a private Unix socket for bounded control frames')
    parser.add_argument('--usbmon-file',
                        help='read a usbmon pcap/text capture instead of a tty')
    parser.add_argument('--usbmon-endpoint', type=int, default=None,
                        help='only keep this IN endpoint number')
    parser.add_argument('--usbmon-control', action='store_true',
                        help='also keep control-transfer payloads from usbmon')
    parser.add_argument('--qr-images', help='decode panel QR/OneSend images from this path')
    parser.add_argument('--qr-decoder', help='path to panel_qr_decode.py')
    parser.add_argument('--replay', metavar='PATH',
                        help='parse a saved capture offline (file or capture directory)')
    parser.add_argument('--force', action='store_true',
                        help='allow --replay to overwrite its output files')
    parser.add_argument('--self-test', action='store_true',
                        help='run the offline self-test and exit')
    return parser


def main(argv=None):
    parser = build_parser()
    arguments = parser.parse_args(argv)

    if arguments.self_test:
        return self_test()
    if arguments.list:
        ports = discover_ports(arguments.serial, arguments.vid, arguments.pid)
        print(json.dumps([port.as_dict() for port in ports], indent=2))
        if not ports:
            print('no candidate CDC-ACM port; is the device enumerated? '
                  'check dmesg for cdc_acm, or capture usbmon', file=sys.stderr)
            return 1
        return 0
    if arguments.replay:
        record = replay(arguments.replay, arguments)
        print(json.dumps(record, indent=2))
        return 0
    if arguments.qr_images and not arguments.output:
        parser.error('--qr-images needs --output')
    if arguments.qr_images and not (arguments.usbmon_file or arguments.port):
        arguments.output.mkdir(parents=True, exist_ok=True)
        code, decoded, message = decode_panel_images(
            arguments.qr_images, arguments.output, arguments.qr_decoder)
        if message:
            print(message, file=sys.stderr)
        if code != 0:
            print('panel decode failed (exit %d)' % code, file=sys.stderr)
            return code
        print('panel log: %s' % decoded)
        return 0
    if not arguments.output:
        parser.error('--output is required')
    arguments.output.mkdir(parents=True, exist_ok=True)

    reserved = list(_RESERVED)
    if arguments.control_socket:
        reserved += ['usb-control.sock', 'usb-control.jsonl']
    existing = [name for name in reserved if os.path.lexists(arguments.output / name)]
    if existing:
        parser.error('capture output already exists (%s); choose a fresh directory'
                     % ', '.join(existing))

    started = iso()
    state = dict(bytes=0, synced=0, sha=hashlib.sha256(), port=None, error=None,
                 control=None, stop=False)
    event_stream = (arguments.output / 'capture-events.jsonl').open('a', buffering=1)

    def events(kind, **fields):
        record = dict(updated_utc=iso(), event=kind)
        record.update(fields)
        event_stream.write(json.dumps(record, sort_keys=True) + '\n')

    def request_stop(signum, frame):
        # Never unwind between a write and its byte/hash accounting: the handler only
        # asks for an orderly stop, which the loop observes at a safe point.
        state['stop'] = True

    previous_sigterm = signal.signal(signal.SIGTERM, request_stop)
    previous_sigint = signal.signal(signal.SIGINT, request_stop)
    writer = DecodedWriter(arguments.output / 'xnu-usb.decoded.log',
                           stamp=not arguments.no_timestamp)
    panel_path = None
    status = 'capture-failed'
    try:
        if arguments.qr_images:
            events('panel-decode-start', images=str(arguments.qr_images))
            code, panel_path, message = decode_panel_images(
                arguments.qr_images, arguments.output, arguments.qr_decoder)
            events('panel-decode-done', code=code, path=str(panel_path), message=message)
            if code != 0:
                print(message, file=sys.stderr)
        if arguments.usbmon_file:
            chunks = load_usbmon(arguments.usbmon_file, arguments.usbmon_endpoint,
                                 arguments.usbmon_control)
            events('usbmon-loaded', file=str(arguments.usbmon_file), chunks=len(chunks))
            with (arguments.output / 'xnu-usb.log').open('wb', buffering=0) as raw:
                for _stamp, chunk in chunks:
                    raw.write(chunk)
                    state['sha'].update(chunk)
                    state['bytes'] += len(chunk)
                    writer.feed(chunk)
                raw.flush()
                os.fsync(raw.fileno())
            state['synced'] = state['bytes']
            writer.flush()
            status = 'usbmon-imported' if state['bytes'] else 'usbmon-empty'
        else:
            with (arguments.output / 'xnu-usb.log').open('wb', buffering=0) as raw:
                status = capture_live(arguments, writer, events, raw, state)
                raw.flush()
                os.fsync(raw.fileno())
            state['synced'] = state['bytes']
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError) as error:
        status = 'capture-failed'
        state['error'] = str(error)
        print('capture failed: %s' % error, file=sys.stderr)
    except KeyboardInterrupt:
        status = 'interrupted'
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        signal.signal(signal.SIGINT, previous_sigint)
        try:
            writer.close()
        except OSError:
            pass
        events('stop', status=status)
        event_stream.close()

    record = dict(status=status, started_utc=started, finished_utc=iso(),
                  port=state['port'], usb_serial=arguments.serial,
                  bytes=state['bytes'], synced_bytes=state['synced'],
                  sha256=state['sha'].hexdigest(), max_bytes=arguments.max_bytes,
                  duration_seconds=arguments.seconds, until_marker=arguments.until_marker,
                  stage=arguments.stage,
                  transport=('usbmon capture import' if arguments.usbmon_file
                             else 'USB CDC ACM; Android/ADB not used for capture'),
                  error=state['error'],
                  decoded_log=str(arguments.output / 'xnu-usb.decoded.log'),
                  panel_log=str(panel_path) if panel_path else None,
                  control_socket_requested=arguments.control_socket,
                  control=state['control'],
                  partition_writes_issued=False)
    atomic_record(arguments.output / 'capture.json', record)
    progress = arguments.output / 'capture-progress.json'
    if progress.exists():
        progress.unlink()
    print(json.dumps(record, indent=2))
    return 0 if state['bytes'] and status != 'capture-failed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
