# ⚠️ Attention

This project is still in an early and rough development phase. While some parts are already functional, the software is not yet stable, optimized, or production-ready.

Below is a list of the main tasks and improvements that still need to be completed before the project can be considered mature.

## TODO
* [ ] Implement an audio, keyboard and joystick controller
* [ ] Test the floppy disk controller
* [ ] Optimize the VS (the VMS language) compiler
* [ ] Create a rom with a VS compiler and an UI for the console
* [ ] Fix known bugs (a lots of bugs, maybe around ~500)
* [ ] Complete software/hardware documentation
* [ ] Make the emulator cross-platform (for now the code is running under Macos)

# Video Machine System

The **Video Machine System (VMS)** is a compact 32-bit microcomputer architecture designed in the spirit of late 1980s home computers, game consoles, and embedded development systems. Although internally employing a modern linear address space, the machine philosophy remains faithful to the simplicity and determinism of classic hardware.

The VMS has been designed around the following principles:

* Minimal instruction set.
* Deterministic execution.
* Memory-mapped IO.

### Contents
- [BSL language](#base-system-language)
- [CPU](#central-processing-unit)
- [Memory system](#memory-system)
- [Interrupt controller](#interrupt-controller)
- [DMA controller](#dma-controller)
- [Floppy controller](#floppy-disk-controller)
- [License](#license)

---

# Base system language

Software for the VMS is written in a custom assembly language designed specifically for the architecture called BSL. While it maps almost one-to-one to the underlying instruction set, the assembler provides a set of compile-time features that significantly improve readability without introducing any runtime overhead.

Rather than treating assembly as a stream of anonymous instructions, the language encourages explicit declaration of resources. Procedures declare the registers they use, memory layouts are described symbolically, and hardware registers are accessed through named structures instead of hardcoded addresses.

```
:alloc
    use r0 u32, r1 u8, r3 u32
    push(r0)
    r3 = abs_get(sys, current_ptr)
    r0 = [r3]

    r0 += r1
    if r0 >= endof(mmap@wram) { hlt }

    [r3] = r0
    pop(r0)
;
```

Memory maps can be declared directly in source code, allowing both RAM and memory-mapped peripherals to be addressed symbolically.

```
layout mmap align
    rom   [rom_size],
    ivt   sizeof ivt,
    sys   sizeof sys,
    ...
```

The same mechanism can also be used to describe application-specific data structures at compile time.

```
layout game align
    padding mmap@wram,
    x 2,
    y 2

r0 = [r3 + game@x]
r1 = [r3 + game@y]
```

The assembler also includes a lightweight compile-time metaprogramming system. Macros may accept variadic arguments, iterate over them, and generate code during assembly, allowing common idioms to remain concise while expanding into ordinary instructions.

```
#define push(...) {
    #forvarg { [sp--] = varg }
}

#define pop(...) {
    #forvarg { varg = [++sp] }
}
```

Since macros are expanded entirely at assembly time, they introduce no abstraction cost.

Functions can freely mix ordinary instructions, compile-time expressions, and symbolic constants.

```
#define video_swap {
    r3 = abs_get(vpu, ctrl)
    r0 = [r3]
    r0 |= vpu_ctrl_swap
    [r3] = r0
}
```

Registers may optionally carry type annotations (u8, u16, u32, s16, ...). They affect the generated machine code.

Interrupt handlers, device drivers, DMA operations, and memory allocation are all written using the same language.

Overall, the language aims to preserve the simplicity and predictability of classic assembly while removing much of its boilerplate. It remains entirely deterministic, produces straightforward machine code, and provides just enough compile-time abstraction to make large programs practical without hiding the underlying hardware.

# CENTRAL PROCESSING UNIT
The processor maintains four general-purpose registers.

```
A    Primary accumulator
X    General-purpose register
Y    General-purpose register
I    Index register
```

The accumulator is intended for arithmetic, logical operations and communication with peripheral hardware.
The X and Y registers provide temporary storage during calculations and address manipulation.
The I register serves as the system index register and is primarily intended for indexed memory addressing.

The processor status is a special register (like instruction pointer) and contains six hardware flags:

**ZERO** <br>
Set whenever the result of an operation equals zero.

**SIGN** <br>
Reflects the most significant bit of the last arithmetic result.

**OVERFLOW** <br>
Indicates signed arithmetic overflow.

**CARRY** <br>
Indicates unsigned carry or borrow.

**INTERRUPT** <br>
Controls interrupt acceptance.

**HALTED** <br>
Indicates that processor execution has stopped until an external event occurs.

## Instruction set
Instructions are organized into functional classes rather than fixed execution units. This arrangement permits a small decoder while maintaining a versatile programming model suitable for systems software, games, and embedded applications.

Most arithmetic instructions operate on the Accumulator Register (A), while X, Y and I provide general-purpose storage and indexed addressing.

Below is the entire instruction set:
```
; Rn = A, X, Y, I
; BLOCK_SYSTEM
NOP
HALT
PUSHF
POPF

INT imm8

MOV SP, Rn
MOV Rn, SP

PUSH Rn
POP Rn

; BLOCK_FLOW
JMP imm32
CALL imm32
RTS
RTI

JMP Rn

JZ imm32
JNZ imm32
JC imm32
JNC imm32
JS imm32
JNS imm32

; BLOCK_MOVE
MOV Rn, Rm

; BLOCK_LOAD_IMM
MOV Rn, imm8
MOV Rn, imm16
MOV Rn, imm32

; BLOCK_LOAD_IDX
MOV Rn, [I + disp8] (8)
MOV Rn, [I + disp8] (16)
MOV Rn, [I + disp8] (32)

; BLOCK_STORE_IDX
ST [I + disp8], Rn (8)
ST [I + disp8], Rn (16)
ST [I + disp8], Rn (32)

; BLOCK_LOAD_IND
MOV Rn, [I] (8)
MOV Rn, [I] (16)
MOV Rn, [I] (32)

; BLOCK_STORE_IND
ST [I], Rn (8)
ST [I], Rn (16)
ST [I], Rn (32)

; BLOCK_ARITH
ADD Rn, Rm
ADC Rn, Rm
SUB Rn, Rm
SBC Rn, Rm

; BLOCK_MULDIV
MUL Rn
IMUL Rn
DIV Rn
IDIV Rn

; BLOCK_LOGIC
AND Rn
OR Rn
XOR Rn

; BLOCK_SHIFT
SHL Rn, Rm
SHR Rn, Rm
ROL Rn, Rm
ROR Rn, Rm

; BLOCK_BIT
BIT Rn, imm8
SET Rn, imm8
RES Rn, imm8

; BLOCK_UNARY
INC Rn
DEC Rn
NOT Rn
NEG Rn

; BLOCK_ALU_IMM
ADD Rn, imm8
ADC Rn, imm8
SUB Rn, imm8
SBC Rn, imm8

SHL Rn, imm8
SHR Rn, imm8
ROL Rn, imm8
ROR Rn, imm8

; BLOCK_FLAGS
CMP Rn, Rm
CMP A, imm
CMP I, imm

STC
CLC
STI
CLI
```

# MEMORY SYSTEM

The VMS employs a unified linear memory model.
Memory is divided into:

**ROM** <br>
Contains executable program code and permanent system resources.

**RAM** <br>
Provides temporary storage for program execution.

**Peripheral Devices** <br>
Mapped directly into the processor address space through the system bus. <br>
Every memory access is performed through a single hardware bus interface. <br>
The processor does not distinguish between memory and hardware registers. <br>

---
# Interrupt Controller

The Interrupt Controller Unit (ICU) is responsible for delivering interrupt requests to the CPU. It provides support for both **Non-Maskable Interrupts (NMI)** and **maskable Interrupt Requests (IRQ)**.

The ICU exposes a small memory-mapped interface that allows software to enable, monitor, and acknowledge interrupt requests.

---

## Memory-Mapped Interface

The ICU exposes the following registers.

| Register             | Access     | Description                                         |
| -------------------- | ---------- | --------------------------------------------------- |
| NMI Status Register  | Read/Write | Indicates the status of the non-maskable interrupt. |
| IRQ Enable Register  | Read/Write | Enables or disables individual IRQ sources.         |
| IRQ Pending Register | Read/Write | Contains the pending IRQ requests.                  |

Accesses outside the register space are considered invalid.

---

## Interrupt Types

The controller manages two classes of interrupts.

### Non-Maskable Interrupt (NMI)

The NMI has the highest priority and cannot be blocked by the CPU interrupt enable flag.

When an NMI is pending:

* It is serviced before any IRQ.
* It is delivered immediately.
* The pending NMI condition is cleared once the interrupt is accepted.

---

### Interrupt Requests (IRQ)

IRQ sources are individually maskable through the IRQ Enable register.

An IRQ is considered **active** only when:

* its pending bit is set, and
* the corresponding enable bit is set.

Disabled IRQs remain pending until they are enabled or cleared by software.

---

## IRQ Priority

When multiple IRQs are active simultaneously, the ICU selects the interrupt with the lowest interrupt number.

This produces a fixed-priority scheme in which lower-numbered IRQs have higher priority than higher-numbered ones.

Only one IRQ is delivered during each interrupt polling operation.

---

## CPU Interrupt Mask

Maskable interrupts are affected by the CPU interrupt enable flag.

When interrupts are disabled by the CPU:

* No IRQ is delivered.
* Pending IRQ requests remain stored in the pending register.

Non-maskable interrupts are not affected by this flag.

---

## Interrupt Acknowledgement

When an IRQ is accepted by the CPU:

* The corresponding pending bit is cleared.
* Control is transferred to the appropriate interrupt vector.

Software does not need to acknowledge an interrupt before it is dispatched by the ICU.

---

## Interrupt Vectors

The controller supports two interrupt vector regions:

* **NMI Vector** — used exclusively for the non-maskable interrupt.
* **IRQ Vector Table** — contains one entry for each IRQ source.
* **INT Vector Table** — contains one entry for each software interrupt source.

For IRQs, the interrupt number determines which entry of the IRQ vector table is used.

---

## Software Usage

A typical software sequence for handling IRQs is:

1. Enable the desired IRQ sources in the IRQ Enable register.
2. Configure the corresponding interrupt handlers in the interrupt vector table.
3. Enable interrupts in the CPU.
4. Wait for interrupt delivery.
5. Execute the interrupt service routine.

---

## Programming Notes

* NMIs always have priority over maskable IRQs.
* Disabled IRQs are not lost; they remain pending until serviced or cleared.
* If multiple IRQs become pending simultaneously, they are serviced one at a time according to their fixed priority.
* Only one IRQ is dispatched during each interrupt polling cycle.

# DMA Controller

The DMA (Direct Memory Access) controller provides a mechanism to transfer data between two memory locations without requiring direct CPU involvement during each individual data movement.

The controller is configured through memory-mapped registers and executes transfers incrementally through a state machine. Each execution step performs part of the transfer process, allowing the DMA to share system resources with other components.

---

## Memory-Mapped Interface

The DMA controller exposes a set of memory-mapped registers used for configuration, monitoring, and transfer setup.

| Register                     | Access     | Description                                         |
| ---------------------------- | ---------- | --------------------------------------------------- |
| Control Register             | Read/Write | Enables the DMA and configures transfer parameters. |
| Status Register              | Read Only  | Reports the current DMA state and possible errors.  |
| Source Address Register      | Read/Write | Starting memory address from which data is read.    |
| Destination Address Register | Read/Write | Starting memory address where data is written.      |
| Transfer Count Register      | Read/Write | Number of bytes to transfer.                        |

Accessing an address outside the DMA register range is considered invalid.

---

## Transfer Configuration

Before starting a transfer, the DMA must be configured with:

* A valid source address.
* A valid destination address.
* A non-zero transfer count.
* A valid transfer width.
* The DMA enable flag set.

The transfer width defines the amount of data moved in each transfer operation.

Supported transfer sizes:

| Width     | Size    |
| --------- | ------- |
| Byte      | 8 bits  |
| Half-word | 16 bits |
| Word      | 32 bits |

Invalid width configurations cause the DMA to stop and report a configuration error.

---

## Transfer Process

The DMA executes transfers using an internal state machine.

A transfer begins when the DMA is enabled and the controller is in the idle state.

The transfer sequence is:

1. Validate the transfer configuration.
2. Mark the DMA as busy.
3. Read data from the source address.
4. Write the data to the destination address.
5. Update the transfer progress.
6. Repeat until the requested amount of data has been moved.

For 32-bit transfers, the operation is split into two 16-bit accesses:

* Lower half-word transfer.
* Higher half-word transfer.

This allows the DMA to perform word transfers using the available bus operations.

---

## DMA Status

The status register provides information about the current DMA condition.

Possible states include:

| Status              | Description                                                 |
| ------------------- | ----------------------------------------------------------- |
| Busy                | A transfer is currently in progress.                        |
| Configuration Error | The transfer parameters are invalid.                        |
| Alignment Error     | A memory alignment problem occurred during a bus operation. |
| Access Error        | The bus rejected a memory access.                           |

The busy status is cleared automatically when the transfer finishes or when an error occurs.

---

## Transfer Completion

When the requested amount of data has been transferred:

* The DMA busy state is cleared.
* The enable flag is automatically disabled.
* The internal state returns to idle.
* A DMA interrupt is generated.

Software can use the interrupt to detect transfer completion without continuously polling the status register.

---

## Error Handling

The DMA can terminate a transfer due to configuration or bus errors.

### Configuration Errors

A configuration error occurs when:

* The transfer count is zero.
* The transfer count is smaller than the selected transfer width.
* The configured transfer width is unsupported.

---

### Alignment Errors

An alignment error occurs when the bus reports that a memory access does not respect the required address alignment.

---

### Access Errors

An access error occurs when the bus denies a read or write operation.

---

When an error occurs:

* The transfer is stopped.
* The DMA is disabled.
* The busy status is cleared.
* The controller returns to the idle state.
* A DMA interrupt is generated.

---

## Software Usage Flow

A typical software sequence for using the DMA controller is:

1. Configure the source address.
2. Configure the destination address.
3. Set the transfer count.
4. Select the transfer width.
5. Enable the DMA.
6. Wait for the DMA interrupt or check the status register.
7. Handle completion or possible errors.

---

## Interrupt Behavior

The DMA generates an interrupt after:

* A successful transfer completion.
* A configuration error.
* A bus access failure.

The interrupt allows the system software to react to DMA events asynchronously.

# FLOPPY DISK CONTROLLER
## Memory-Mapped I/O Registers

The FDC exposes its interface to the host system via memory-mapped registers. All interactions with the FDC must happen by reading or writing to these specific addresses.

| Register Name | Mode | Purpose | Description |
| --- | --- | --- | --- |
| `FDC_DATA_ADDR` | Read / Write | Data Input/Output | Write command bytes, arguments, and sector bytes. Read data bytes or status result packets. |
| `FDC_CTRL_ADDR` | Read / Write | Control Register | Manages motor state, active head selection, and DMA behavior. |
| `FDC_STATUS_ADDR` | Read Only | Status Register | High-level controller state flags (busy, data request, errors). |
| `FDC_ERROR_ADDR` | Read Only | Error Register | Contains the specific error code when an operation fails. |
| `FDC_TRACK_ADDR` | Read Only | Current Track | Reflects the physical track location of the drive head. |
| `FDC_SECTOR_ADDR` | Read Only | Current Sector | Reflects the rotational sector currently beneath the drive head. |
| `FDC_DMA_ADDR` | Read / Write | DMA Target Address | A 32-bit (word) pointer configuring where DMA transfers point to. |

### Register Bitmasks

#### Control Register (`FDC_CTRL_ADDR`)

* `FDC_CTRL_MOTOR`: Drive motor state (1 = ON, 0 = OFF).
* `FDC_CTRL_HEAD`: Active head selection
* `FDC_CTRL_DMA_ENABLED`: Toggles data transfer mode (1 = DMA Enabled, 0 = Programmed I/O Mode).

#### Status Register (`FDC_STATUS_ADDR`)

* `FDC_STATUS_BUSY`: Set when processing a command sequence or mechanical movement.
* `FDC_STATUS_DRQ`: Data Request flag. Indicates a byte is ready to read or write in `FDC_DATA_ADDR`.
* `FDC_STATUS_ERROR`: Latches to high when an operation encounters a failure.
* `FDC_STATUS_TRACK0`: Active when the physical head rests at Track 0.
* `FDC_STATUS_SEEK_DONE`: Indicates a Seek or Recalibrate sequence finished successfully.
* `FDC_STATUS_INDEX`: Pulled high briefly when the disk completes a full rotation.
* `FDC_STATUS_DISK_IN`: Reflects whether a floppy image is safely inside the drive.
* `FDC_STATUS_WRITE_PROTECTED`: Drive is read-only.

---

## Command Set Reference

Commands are dispatched by writing a single command byte directly to `FDC_DATA_ADDR`. For multi-byte command strings, any required arguments must follow sequentially through the same address register.

### `FDC_CMD_RECAL` (0x01)

Resets the drive head back to physical Track 0, spins up the motor, and establishes baseline track coordinates.

* **Arguments:** None.
* **Interrupt Behavior:** Raises an IRQ when `FDC_STATUS_TRACK0` and `FDC_STATUS_SEEK_DONE` flags latch.

### `FDC_CMD_SEEK` (0x02)

Steps the physical drive head to a specific destination track.

* **Arguments:** 1 Byte (`Track Number`).
* **Interrupt Behavior:** Raises an IRQ once the head aligns over the destination track.

### `FDC_CMD_READ` (0x03)

Reads a block from the virtual track/sector layout into memory. Spun-down motors will automatically trigger a spin-up delay prior to alignment.

* **Arguments:** 2 Bytes (`Track Number`, `Sector Number`).
* **Interrupt Behavior:** Raises an IRQ when the final sector byte clears the transfer buffer.

### `FDC_CMD_WRITE` (0x04)

Writes a block from memory to the virtual layout. Fails immediately if the drive latches `FDC_STATUS_WRITE_PROTECTED`.

* **Arguments:** 2 Bytes (`Track Number`, `Sector Number`).
* **Interrupt Behavior:** Raises an IRQ when all sector data hits the virtual platter structure.

### `FDC_CMD_FORMAT` (0x05)

Wipes a target track on the current head setting, writing a standard `0xE5` filler throughout all sectors on that path.

* **Arguments:** 1 Byte (`Track Number`).
* **Interrupt Behavior:** Immediately signals an IRQ on completion of one virtual disk rotation (`FDC_ROTATION_TIME_NS`).

### `FDC_CMD_STATUS` / `FDC_CMD_ID` (0x06 / 0x07)

Requests a 5-byte physical context diagnostic packet from the controller.

* **Arguments:** None.
* **Result Packet Contents:**
1. Byte 0: `Current Track`
2. Byte 1: `Current Head`
3. Byte 2: `Current Sector`
4. Byte 3: `Disk Present Flag` (Non-zero if disk is in)
5. Byte 4: `Write Protected Flag` (Non-zero if read-only)



---

## Communication Protocols

### Command Sequence Flow

All operations must strictly respect the state transitions managed by the controller. Attempting to issue a new command while the controller is handling previous steps will lead to data corruption or missing cycles.

```
[IDLE State] 
     │
     ▼
Write Command Byte ──► [FDC_STATUS_BUSY set]
     │
     ├──► Requires 1 Arg?  ──► Write Arg 0
     │
     ├──► Requires 2 Args? ──► Write Arg 0 ──► Write Arg 1
     │
     ▼
[Execution Phase]

```

### Data Transfer Mechanics

The FDC handles bulk memory data paths using one of two mutually exclusive execution profiles selected in `FDC_CTRL_ADDR`.

#### A. Programmed I/O (PIO Mode)

When `FDC_CTRL_DMA_ENABLED` is cleared (0), data must be streamed byte-by-byte through CPU loops.

1. Issue a `READ` or `WRITE` sequence.
2. Poll `FDC_STATUS_ADDR` until `FDC_STATUS_DRQ` evaluates to high.
3. Read or write exactly one byte at `FDC_DATA_ADDR`.
4. Repeat this loop for the entire block (FDC\_SECTOR\_SIZE).

> ⚠️ **CRITICAL TIMEOUT WARNING:** Once the controller asserts `FDC_STATUS_DRQ`, the host CPU must read or write the data byte before the internal `FDC_DRQ_TIMEOUT_NS` duration expires. Failing to service the register in time forces the controller to abort the execution thread and flag a `FDC_ERROR_LOST_DATA` error code.

#### B. Direct Memory Access (DMA Mode)

When `FDC_CTRL_DMA_ENABLED` is high (1), data paths are offloaded to an internal DMA channel.

1. Populate the target memory physical base address into `FDC_DMA_ADDR`.
2. Dispatch your `READ` or `WRITE` sequence.
3. The controller asserts `FDC_STATUS_DRQ`, automatically programs the source/destination bindings into the system DMA controller, and engages `DMA_CTRL_ENABLE`.
4. Wait for the hardware interrupt line (`IVT_IRQ_FDC`) to pull high, indicating full sector delivery.

---

## Hardware Status & Error Recovery

When things break, the FDC drops out of execution states, wipes the active `FDC_STATUS_BUSY` and `FDC_STATUS_DRQ` status bits, places a specific error signature inside `FDC_ERROR_ADDR`, and throws a target CPU interrupt.

### Register Recovery Mapping

When catching an error interrupt, evaluate the content byte found inside `FDC_ERROR_ADDR`:

* `FDC_ERROR_INVALID_COMMAND`: The controller received a byte sequence it didn't recognize.
* `FDC_ERROR_SEEK`: The execution loop requested a target track index that exceeds bounds (`FDC_TRACKS`).
* `FDC_ERROR_RECORD_NOT_FOUND`: The requested target sector argument evaluates to 0 or overflows beyond bounds (`FDC_SECTORS`). Also fires if an inner disk data access window crosses outside the physical image file size.
* `FDC_ERROR_NOT_READY`: Executing search loops on the track when `FDC_STATUS_DISK_IN` is missing.
* `FDC_ERROR_WRITE_PROTECT`: A write or format command hit the pipeline while `FDC_STATUS_WRITE_PROTECTED` was locked.
* `FDC_ERROR_LOST_DATA`: The host processor failed to process an active `DRQ` cycle within the required hardware clock window.

# LICENSE

VMS is licensed under the MIT License. See the [`LICENSE`](LICENSE) file for more details.