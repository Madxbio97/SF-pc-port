# H2 CD-ROM and continuous-loop findings

The supported target remains `SCUS-94240` USA v1.1.

- CPU clock: 33,868,800 Hz.
- CD command ACK: 25,000 ticks (`Init`: 80,000).
- Sector cadence: 451,584 ticks at 1x; 225,792 at 2x.
- IRQ reasons: INT1 data, INT2 complete, INT3 acknowledge, INT4 end, INT5 error;
  IEN is a one-hot mask for those encoded reasons.
- SCUS raw reads use mode `0xa0`: 12-byte MODE2 header, 2048 data bytes and tail.
  The guest consumes the header before DMA3 copies exactly `0x200` words.
- DMA3 uses `BCR=0x00010200`, `CHCR=0x11000000` and requires the CD request bit.
  Invalid or stale internal DMA3 requests abort atomically without consuming the
  FIFO, changing RAM/MADR or leaving CHCR busy.

`LegacyVirtualCd` remains an immutable host catalog plus serialized handle state.
Its open-file reads are planned first; cursor advancement is committed only after
the CD-ROM/DMA3 transfer succeeds. CD registers, FIFO contents, LBA, IRQ state and
event generations live in `PsxMachineState`.

Automated gates:

```text
sf_r3000_runtime_tests
sf_tool probe-legacy-cd <game.cue>
sf_tool probe-legacy-loop <game.cue>
```

The first gate covers MMIO/IRQ/MODE2/DMA3, failure recovery and active-IRQ/
mid-sector snapshot replay. The ROM gate verifies unchanged `SUBWAY.OVL` code
after hardware loading and a clean I_STAT boundary. The loop gate boots the real
executable, returns from each native GPU host call, pulses VBlank and resumes the
same CPU until the guest VBlank counter advances.
