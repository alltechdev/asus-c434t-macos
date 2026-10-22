# C434TeMMCIRQ (diagnostic kext)

Not part of the shipped EFI. It was written to find out why the eMMC had no
interrupt under macOS, and it is kept here because it is also a working example
of building a kext entirely on Linux.

It probes the eMMC (`8086:9d2b`) ahead of EmeraldSDHC, records what the PCI
family sees into a `C434TeMMCIRQ` property on the device, retries the
`ResolvePCIInterrupt` lookup and, if needed, calls `SetDeviceInterrupts` itself.
`probe()` always declines, so EmeraldSDHC still attaches normally. Its output is
what pinned the problem down:

```
EMMC dev 0x1e pin 2 line 0xff, 0 interrupt specifier(s) before;
ResolvePCIInterrupt(dev 0x1e, pin 1) on PCI0 -> 0x0, irq 21;
SetDeviceInterrupts(21) -> 0xe00002df (kIOReturnNoInterrupt)
```

i.e. routing was fine; GSI 21 was already taken (by HPET). See
[../docs/FINDINGS.md](../docs/FINDINGS.md).

## Building on Linux

Needs clang, Apple's `ld64` (conda-forge `ld64_osx-64`, runs on Linux) and
acidanthera's MacKernelSDK, placed in `../toolchain`:

```sh
mkdir -p toolchain && cd toolchain
curl -sL https://micro.mamba.pm/api/micromamba/linux-64/latest | tar -xj bin/micromamba
MAMBA_ROOT_PREFIX=$PWD/mroot ./bin/micromamba create -y -p $PWD/env -c conda-forge ld64_osx-64
git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git
cd .. && sh kext/build.sh
```

Upstream clang needs two things Apple's toolchain does by default, or OpenCore
refuses to prelink the kext (`Symbol ___cxa_atexit has 0-value`):
`-fno-register-global-dtors-with-atexit` and
`-mllvm --disable-atexit-based-global-dtor-lowering`, so static destructors go to
`__mod_term_func` instead of `__cxa_atexit`, which the kernel does not export.
