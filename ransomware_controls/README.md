# Ransomware guard fixtures

`fixture.cpp` is a standalone AES sample. Build with the installed MSVC toolchain:

```cmd
mkdir bin
cl /nologo /W4 /WX /EHsc fixture.cpp /Fo:bin\fixture.obj /Fe:bin\fixture.exe
```

It creates sixteen new 1 KiB files in a fresh GUID directory beneath the effective
temporary directory. It has no input-directory option, file enumeration,
encryption, networking, or deletion. Keep the printed directory for independent
content verification, then remove that exact task-owned directory after removing
the policy. Set TEMP/TMP to the task workspace before running.

The READY line reports the PID and a 15-second interval for applying VOL. Select
one of `burst`, `low`, `already-high`, `single-file`, `slow`, `failed-write`.
The high-entropy payload is the public byte sequence 0..255 repeated four times.
It exercises entropy measurement without implementing a ransomware encryptor.

For windowMs=1000, minimumFiles=4, minimumWrites=4, minimumBytes=4096,
minimumEntropyQ8=1792 and minimumDeltaQ8=512:

| Case | Expected detector | Expected writes |
|---|---|---|
| Disabled + burst | None | 16 permitted |
| Audit + burst | One entropy-burst finding | 16 permitted |
| Block + burst | One entropy-burst finding | First 4 complete, later writes denied |
| Low/already-high/single-file/slow | None | 16 permitted |
| Failed-write | None | 16 fail through ordinary file access checks |
| Five-second TTL + burst after expiry | None | 16 permitted |
| Block + two-file/two-write/2048-byte threshold | One entropy-burst finding | First 2 complete, later writes denied |

The accompanying manifest contains these ten cases. Pass the AES repository as
`--suite-root` and this manifest explicitly to the candidate runner's
`--detection-audit` mode. The fixture binary path is relative to that suite root.
Use a task-specific TEMP/TMP directory and output directory. The checked-in
manifest deliberately does not acknowledge a snapshot or enable the lab override:
prepare a task-local manifest with `launch.volSnapshotAcknowledged=true` only
after verifying the snapshot, or `launch.volAllowLabUnsafe=true` after explicit
operator authorization. Apply that setting only to the VOL-enabled cases.

The current runner requires `useUsermodeHooks=true` to establish analysis
ownership for VOL; the detector itself executes in the kernel. In the expiry
case, rollback expects zero policies because the applied policy has already
expired. Independently verify that policy application succeeded before accepting
that negative result.

Also test live policy removal/replacement, a separate untargeted process, resource
exhaustion, and concurrent I/O before expanding deployment. Expected results here
are specifications, not recorded
runtime results. This sample does not cover mapped writes, offloaded copies,
cross-process transformations, or identify a cryptographic algorithm.
