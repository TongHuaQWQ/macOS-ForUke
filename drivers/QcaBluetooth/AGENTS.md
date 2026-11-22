# QcaBluetooth

This component owns Qualcomm Bluetooth initialization, H4/IBS, GENI UART DMA,
the native UART.SK transport and the BlueTool bootstrap service. Board wiring,
shared WCN power and firmware selection belong to the device repository.

Read `README.md`, `tools/source-lock.json` and the consuming device's
`AGENTS.md` before changing code. Preserve the locked Linux sources. Put
compatibility services in `shim/` and namespace every exported helper.

Follow UniXNU's `CONTRIBUTING.md`. Run the affected existing host check and
compile changed kernel sources against the selected target's recorded command.
Keep simulated results, target compilation and device observations distinct.
Update the English and Chinese UniXNU driver pages and the device record.
