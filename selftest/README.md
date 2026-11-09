# selftest

A small round-trip check of the OP2 **discovery** protocol used by `OP2LanScan` / `op2finder`.
It builds the 42-byte discovery query and validates the checksum and field layout against the
spec, and cross-checks against the live implementation by reflecting into the built
`OP2LanScan.exe` (`OP2LanScan.Op2Discovery`).

## Files
- `Test.cs` / `Test.exe` - the round-trip self-test (build query, verify checksum + offsets).
- `Probe.cs` / `Probe.exe` - reflects into the app assembly to compare its query bytes.

## Run
Both `Test.cs` and `Probe.cs` hard-code the app's **Debug** exe path, so build OP2LanScan in
Debug once first:
```
MSBuild OP2LanScan\OP2LanScan\OP2LanScan.csproj /p:Configuration=Debug /p:Platform=AnyCPU
```
Then compile and run the test with the .NET Framework C# compiler (`csc`), e.g.:
```
csc Test.cs && Test.exe
```

Reference: the byte-exact discovery spec is [`..\DISCOVERY_PROTOCOL.md`](../DISCOVERY_PROTOCOL.md).

> Note: `Test.exe` / `Probe.exe` and `*.png` here are dev artifacts (git/svn-ignored).
