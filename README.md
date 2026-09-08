# PowerBoard26 firmware

PowerBoard26 targets the STM32G491 and participates in the avionics SEDSNet
CAN-FD network. It uses SEDSNet v4.0.20 and SEDS LaunchCore v1.0.0, fetched by
CMake without submodules. Linker scripts, bootloader placement, firmware slot,
OTA delta storage, and persistent-data storage are derived from
`Bootloader/board_config.h`.

## Build, flash, and OTA

```sh
./build.py build --release
./build.py flash --release --method stm32prog-cli
./build.py clean
```

The normal flash command writes the combined `.factory.bin` at `0x08000000`.
Use `./build.py flash --help` for the methods available on the host. OTA output
uses `.seds`: it contains a delta when the board layout and previous packaged
image allow one, otherwise it uses LaunchCore recovery for a full image.

The SEDSNet runtime schema is `config/sedsnet.json`. Managed underglow and
flight-state values are restored from the LaunchCore-approved persistent region
before network synchronization; later network updates replace the local value
and are persisted without using the firmware or OTA slots.

## Tests

```sh
./build.py test
./build.py test --all --release
```

The full suite adds release firmware/bootloader and OTA builds, memory-limit
checks, simulated peripherals and faults, long-duration allocator probes, and
linked SEDSNet discovery, synchronization, managed-variable, and command tests.
The simulated STM32G491 limits in `sim/board.json` are distinct from the
SEDSNet pool limit and both are enforced.


## Regenerating with STM32CubeMX

Open the checked-in `.ioc` file and generate with the CMake toolchain. Keep user
code enabled. The `.ioc` is the source of truth for the ThreadX and USBX pool
sizes; unit tests compare those values with the generated Azure RTOS headers so
regeneration cannot silently shrink, grow, or repartition the pools.

The top-level CMake project is board-owned and reconnects generated STM32
sources with SEDSNet, LaunchCore, its generated linker scripts, persistence, and
the simulator probes. After generation, run
`python3 build.py test --full --release` before flashing or committing.
