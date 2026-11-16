# QcomDwc3

This component owns the Qualcomm DWC3 subclasses of Apple's USB device and host
controllers, the eUSB2 and QMP PHY sequences and the UCSI client. Board rails,
the SMMU context, the charger channel and prelink roots belong to the device
repository.

Read `README.md` and the consuming device's `AGENTS.md` before changing code.
Follow mainline Linux for hardware sequences and leave USB behavior to Apple's
stacks.

Follow UniXNU's `CONTRIBUTING.md`. Build `tests/ucsi_host.cpp` and run it after
UCSI changes. Keep host results and device observations distinct. Update the
English and Chinese UniXNU driver pages and the device record.
