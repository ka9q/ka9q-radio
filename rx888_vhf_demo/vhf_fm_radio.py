#!/usr/bin/env python3
"""
vhf_fm_radio.py — Interactive TUI for RX888 VHF FM-broadcast operation.

A Textual terminal UI for live frequency hopping, gain control, and bandwidth
cycling on the RX888 mk2 VHF front-end. Self-contained: does the full init
on launch (enter VHF, CLKB on, R828D init, initial tune) and standby on quit.
No prior vhf_tune.py run required.

Requires: textual (pip install textual), pyusb (pip install pyusb)
"""

import argparse, io, queue, sys
try:
    import usb.core
except ImportError:
    usb = None
try:
    from textual.app import App, ComposeResult
    from textual.widgets import Static, Rule, RichLog
    from textual.containers import Container, Vertical
except ImportError:
    sys.exit("textual is required: pip install textual")


class _StdoutTee(io.TextIOBase):
    """Intercept stdout into a thread-safe queue for the TUI log panel."""
    def __init__(self, original):
        self._orig = original
        self._q = queue.Queue()

    def write(self, s):
        if s and s.strip():
            self._q.put(s)
        return len(s) if s else 0

    def flush(self):
        pass

    def drain(self):
        lines = []
        while not self._q.empty():
            try:
                lines.append(self._q.get_nowait())
            except queue.Empty:
                break
        return lines

from rx888_vhf import (
    RX888, TuneError, firmware_load,
    IF_CARRIER, R828D_REF_HZ, BW_CYCLE,
)

# ── Tuning limits ──────────────────────────────────────────────────────
R828D_MIN_HZ = 24_000_000       # ~24 MHz
R828D_MAX_HZ = 1_766_000_000    # ~1.766 GHz
FM_LO_HZ     = 88_100_000
FM_HI_HZ     = 107_900_000

HELP_TEXT = """\
[dim]\u2190 \u2192[/]  freq \u00b1100 kHz    [dim]PgUp/Dn[/]  \u00b11 MHz
[dim]Home/End[/]  FM band edges (88.1 / 107.9 MHz)
[dim]b / B[/]  cycle bandwidth (narrower / wider)
[dim]l / L[/]  LNA \u00b11          [dim]m / M[/]  mixer \u00b11
[dim]v / V[/]  VGA \u00b11          [dim]a / A[/]  LNA / mixer AGC
[dim]f[/]  toggle channel filter (PWD_FILT)
[dim]e[/]  toggle FILTER_EXT   [dim]w[/]  toggle FLT_EXT_WIDEST
[dim]i / I[/]  IF offset \u00b1100 kHz (probe filter edges)
[dim]0[/]  reset IF offset     [dim]r[/]  toggle ref 16/32 MHz
[dim]p / P[/]  cal park \u00b110 MHz (probe filter corner vs park)
[dim]q[/]  quit"""


class VHFRadioApp(App):
    ALLOW_SELECT = True

    CSS = """
    Screen {
        align: center middle;
    }
    #outer {
        width: 90;
        height: auto;
        max-height: 100%;
    }
    #panel {
        border: solid green;
        padding: 1 2;
    }
    #title {
        text-align: center;
        text-style: bold;
        color: $accent;
        margin-bottom: 1;
    }
    #status {
        margin-top: 1;
    }
    #help {
        color: $text-muted;
    }
    #log {
        height: 12;
        border: solid $accent-darken-2;
        margin-top: 1;
    }
    """

    def __init__(self, freq_hz, ref_hz, bias_tee, base, force):
        super().__init__()
        self._start_freq = freq_hz
        self._ref_hz = ref_hz
        self._bias_tee = bias_tee
        self._arg_base = base
        self._force = force
        self._rx = None
        self._base = None
        self._hw_ready = False
        self._freq_hz = freq_hz
        self._bw_mhz = 8
        self._if_hz = IF_CARRIER
        self._lna = 0
        self._mixer = 0
        self._vga = 11
        self._locked = False
        self._lna_agc = False
        self._mixer_agc = False
        self._chan_filt = True       # PWD_FILT=1 (init 0x0A=0xDB, bit7=1)
        self._filt_ext = False      # FILTER_EXT (0x1E[6]); set by BW preset
        self._filt_ext_w = False    # FLT_EXT_WIDEST (0x0F[7]); never touched
        self._if_offset = 0         # IF probe offset from nominal (Hz)
        self._cal_code = None       # FILT_CODE from calibrate_filter()
        self._cal_park = 100_100_000  # cal PLL park freq (non-round → sdm≠0)
        self._status_msg = ""

    def compose(self) -> ComposeResult:
        with Vertical(id="outer"):
            with Container(id="panel"):
                yield Static("RX888 VHF FM Radio", id="title")
                yield Static("", id="freq")
                yield Static("", id="bw")
                yield Static("", id="lna_bar")
                yield Static("", id="mixer_bar")
                yield Static("", id="vga_bar")
                yield Static("", id="chan_filt")
                yield Static("", id="filt_ext")
                yield Static("", id="status")
                yield Rule()
                yield Static(HELP_TEXT, id="help")
            yield RichLog(id="log", max_lines=200, markup=True)

    def on_mount(self) -> None:
        self._tee = _StdoutTee(sys.stdout)
        sys.stdout = self._tee
        self.set_interval(0.1, self._drain_log)
        self._update_display()
        self.run_worker(self._hw_init, thread=True)

    def _drain_log(self) -> None:
        if not hasattr(self, '_tee'):
            return
        log = self.query_one("#log", RichLog)
        for line in self._tee.drain():
            log.write(line.rstrip())

    def _hw_init(self) -> None:
        try:
            self._rx = RX888()
            self._rx.check_alive()
            if self._arg_base is not None:
                self._base = self._arg_base
            else:
                base = self._rx.read_gpio_state()
                if base is None:
                    raise TuneError(
                        "GETSTATS does not expose GPIO state; pass --base")
                self._base = base
            self._rx.enter_vhf(self._base, self._bias_tee)
            self._rx.clkb_on(self._ref_hz)
            if not self._rx.clkb_verify() and not self._force:
                raise TuneError("CLKB reads back as OFF (use --force)")
            idv = self._rx.r828d_probe()
            if idv is None:
                raise TuneError("R828D not reachable over I2C")
            if idv != 0x96 and not self._force:
                raise TuneError(f"R828D ID 0x{idv:02X} != 0x96 (use --force)")
            self._rx.r828d_init()        # includes calibrate_filter(); PLL parked at default
            self._cal_code = self._rx.regs.get(0x0A, 0) & 0x0F
            self._locked = self._rx.r828d_set_freq(int(self._freq_hz))
            gains = self._rx.get_gains()
            self._lna = gains['lna']
            self._mixer = gains['mixer']
            self._vga = gains['vga']
            agc = self._rx.get_agc()
            self._lna_agc = agc['lna']
            self._mixer_agc = agc['mixer']
            self._chan_filt = self._rx.get_chan_filter()
            self._filt_ext = self._rx.get_filt_ext()
            self._filt_ext_w = self._rx.get_filt_ext_widest()
            self._hw_ready = True
            self.call_from_thread(self._update_display)
        except (TuneError, SystemExit) as e:
            if self._rx and self._base is not None:
                try:
                    self._rx.standby(self._base)
                except Exception:
                    pass
            self.call_from_thread(self._show_fatal, str(e))

    def _show_fatal(self, msg: str) -> None:
        self._status_msg = f"[red]FATAL: {msg}[/]"
        self._update_display()

    def _update_display(self) -> None:
        lock_str = ("[green]● LOCKED[/]" if self._locked
                    else "[red]○ unlocked[/]")
        self.query_one("#freq", Static).update(
            f"  Frequency   [bold]{self._freq_hz / 1e6:.3f}[/] MHz"
            f"         {lock_str}")
        bw_str = (f"{self._bw_mhz:g} MHz" if self._bw_mhz >= 1
                  else f"{int(self._bw_mhz * 1000)} kHz")
        actual_if = self._if_hz + self._if_offset
        if self._if_offset:
            sign = "+" if self._if_offset > 0 else ""
            off_str = f"  [cyan]{sign}{self._if_offset // 1000}k[/]"
        else:
            off_str = ""
        ref_mhz = self._ref_hz // 1_000_000
        ref_str = (f"  [cyan]ref {ref_mhz}M[/]"
                   if ref_mhz != 16 else f"  ref {ref_mhz}M")
        self.query_one("#bw", Static).update(
            f"  Bandwidth   {bw_str}"
            f"    IF {actual_if / 1e6:.3f} MHz{off_str}{ref_str}")

        def bar(label, val, agc=False, max_val=15):
            filled = "\u2588" * val + "\u2591" * (max_val - val)
            agc_tag = "  [green]AGC[/]" if agc else ""
            return f"  {label:6s} {filled} {val:2d} / {max_val}{agc_tag}"

        self.query_one("#lna_bar", Static).update(
            bar("LNA", self._lna, self._lna_agc))
        self.query_one("#mixer_bar", Static).update(
            bar("Mixer", self._mixer, self._mixer_agc))
        self.query_one("#vga_bar", Static).update(bar("VGA", self._vga))

        filt_str = ("[green]ON[/]" if self._chan_filt
                    else "[yellow]OFF (raw mixer IF)[/]")
        ext_tags = []
        if self._filt_ext:
            ext_tags.append("EXT")
        if self._filt_ext_w:
            ext_tags.append("WIDEST")
        ext_str = ("  [cyan]+" + "+".join(ext_tags) + "[/]" if ext_tags
                   else "")
        self.query_one("#chan_filt", Static).update(
            f"  Filter {filt_str}{ext_str}")
        cal_str = (f"cal={self._cal_code}"
                   if self._cal_code is not None else "cal=?")
        park_str = f"@{self._cal_park / 1e6:.1f}M"
        self.query_one("#filt_ext", Static).update(
            f"  FILTER_EXT {'[cyan]ON[/]' if self._filt_ext else '[dim]off[/]'}"
            f"    FLT_EXT_WIDEST {'[cyan]ON[/]' if self._filt_ext_w else '[dim]off[/]'}"
            f"    [dim]{cal_str} {park_str}[/]")

        status = self.query_one("#status", Static)
        if self._status_msg:
            status.update(self._status_msg)
        elif not self._hw_ready:
            status.update("[dim]Initializing...[/]")
        else:
            status.update("")

    # ── key handling ──────────────────────────────────────────────────────
    def on_key(self, event) -> None:
        char = event.character
        key = event.key

        # quit is always available
        if char == "q":
            self._cleanup_and_exit()
            return

        if not self._hw_ready:
            return

        if key == "left":
            self._change_freq(-100_000)
        elif key == "right":
            self._change_freq(100_000)
        elif key in ("pageup", "page_up"):
            self._change_freq(1_000_000)
        elif key in ("pagedown", "page_down"):
            self._change_freq(-1_000_000)
        elif key == "home":
            self._set_freq(FM_LO_HZ)
        elif key == "end":
            self._set_freq(FM_HI_HZ)
        elif char == "b":
            self._cycle_bw(1)
        elif char == "B":
            self._cycle_bw(-1)
        elif char == "a":
            self._toggle_agc("lna")
        elif char == "A":
            self._toggle_agc("mixer")
        elif char == "l":
            self._adj_gain("lna", 1)
        elif char == "L":
            self._adj_gain("lna", -1)
        elif char == "m":
            self._adj_gain("mixer", 1)
        elif char == "M":
            self._adj_gain("mixer", -1)
        elif char == "v":
            self._adj_gain("vga", 1)
        elif char == "V":
            self._adj_gain("vga", -1)
        elif char == "f":
            self._toggle_chan_filter()
        elif char == "e":
            self._toggle_filt_ext()
        elif char == "w":
            self._toggle_filt_ext_widest()
        elif char == "i":
            self._nudge_if(100_000)
        elif char == "I":
            self._nudge_if(-100_000)
        elif char == "0":
            self._reset_if_offset()
        elif char == "r":
            self._toggle_ref()
        elif char == "p":
            self._nudge_cal_park(10_000_000)
        elif char == "P":
            self._nudge_cal_park(-10_000_000)

    def _change_freq(self, delta_hz: int) -> None:
        self._set_freq(self._freq_hz + delta_hz)

    def _set_freq(self, hz: int) -> None:
        hz = max(R828D_MIN_HZ, min(R828D_MAX_HZ, hz))
        if hz == self._freq_hz:
            return
        self._freq_hz = hz
        self._status_msg = ""
        self._locked = self._rx.r828d_set_freq(int(hz))
        self._update_display()

    def _adj_gain(self, stage: str, delta: int) -> None:
        attr = f"_{stage}"
        val = getattr(self, attr) + delta
        if not 0 <= val <= 15:
            return
        setattr(self, attr, val)
        getattr(self._rx, f"set_{stage}_gain")(val)
        self._update_display()

    def _cycle_bw(self, direction: int = 1) -> None:
        idx = (BW_CYCLE.index(self._bw_mhz) + direction) % len(BW_CYCLE)
        new_bw = BW_CYCLE[idx]
        old_if = self._if_hz
        new_if = self._rx.set_bandwidth(new_bw)
        self._bw_mhz = new_bw
        self._if_hz = new_if
        self._if_offset = 0         # new filter shape; reset probe offset
        # set_bandwidth overwrites 0x1E[6] and FILT_CODE; resync + recal
        self._filt_ext = self._rx.get_filt_ext()
        self._cal_code = self._rx.calibrate_filter(self._cal_park)
        if new_if != old_if:
            self._status_msg = (
                f"[yellow]IF shifted: ka9q channel "
                f"{old_if // 1000}k \u2192 {new_if // 1000}k[/]")
        else:
            self._status_msg = ""
        self._locked = self._rx.r828d_set_freq(int(self._freq_hz))
        self._update_display()

    def _nudge_if(self, delta_hz: int) -> None:
        """Shift IF offset (filter stays fixed, LO moves). Probes passband edges."""
        self._if_offset += delta_hz
        self._rx.if_hz = self._if_hz + self._if_offset
        self._locked = self._rx.r828d_set_freq(int(self._freq_hz))
        self._status_msg = ""
        self._update_display()

    def _reset_if_offset(self) -> None:
        if self._if_offset == 0:
            return
        self._if_offset = 0
        self._rx.if_hz = self._if_hz
        self._locked = self._rx.r828d_set_freq(int(self._freq_hz))
        self._status_msg = ""
        self._update_display()

    def _nudge_cal_park(self, delta_hz: int) -> None:
        """Shift cal park ±10 MHz (70.1–110.1 MHz, all mix_div=32, non-integer)."""
        new = self._cal_park + delta_hz
        if not (70_100_000 <= new <= 110_100_000):
            return
        self._cal_park = new
        self._cal_code = self._rx.calibrate_filter(self._cal_park)
        self._locked = self._rx.r828d_set_freq(int(self._freq_hz))
        cs = f"cal={self._cal_code}" if self._cal_code is not None else "cal=FAIL"
        self._status_msg = f"[cyan]cal park \u2192 {self._cal_park/1e6:.1f} MHz  {cs}[/]"
        self._update_display()

    def _toggle_ref(self) -> None:
        """Toggle CLKB reference between 16 and 32 MHz, reprogram Si5351,
        recalibrate filter against new reference, retune."""
        if self._ref_hz == 16_000_000:
            self._ref_hz = 32_000_000
        else:
            self._ref_hz = 16_000_000
        self._rx.clkb_on(self._ref_hz)
        self._cal_code = self._rx.calibrate_filter(self._cal_park)
        self._locked = self._rx.r828d_set_freq(int(self._freq_hz))
        cal_str = f" cal={self._cal_code}" if self._cal_code is not None else " cal=FAIL"
        self._status_msg = f"[cyan]Ref \u2192 {self._ref_hz // 1_000_000} MHz{cal_str}[/]"
        self._update_display()

    def _toggle_chan_filter(self) -> None:
        self._chan_filt = not self._chan_filt
        self._rx.set_chan_filter(self._chan_filt)
        self._status_msg = (
            "" if self._chan_filt
            else "[yellow]PWD_FILT=0: filter bypassed (raw mixer IF)[/]")
        self._update_display()

    def _toggle_filt_ext(self) -> None:
        self._filt_ext = not self._filt_ext
        self._rx.set_filt_ext(self._filt_ext)
        self._status_msg = (
            "[cyan]FILTER_EXT on (BW change will override)[/]"
            if self._filt_ext else "")
        self._update_display()

    def _toggle_filt_ext_widest(self) -> None:
        self._filt_ext_w = not self._filt_ext_w
        self._rx.set_filt_ext_widest(self._filt_ext_w)
        self._status_msg = (
            "[cyan]FLT_EXT_WIDEST on[/]" if self._filt_ext_w else "")
        self._update_display()

    def _toggle_agc(self, stage: str) -> None:
        attr = f"_{stage}_agc"
        now = not getattr(self, attr)
        setattr(self, attr, now)
        getattr(self._rx, f"set_{stage}_agc")(now)
        self._update_display()

    def _cleanup_and_exit(self) -> None:
        if hasattr(self, '_tee'):
            sys.stdout = self._tee._orig
        if self._rx and self._base is not None:
            try:
                self._rx.standby(self._base)
            except Exception:
                pass
        self.exit()


def parse_args():
    ap = argparse.ArgumentParser(
        description="RX888 VHF FM Radio — interactive TUI")
    ap.add_argument("--freq", type=float, default=FM_LO_HZ,
                    help=f"initial frequency in Hz (default {FM_LO_HZ/1e6:.1f} MHz)")
    ap.add_argument("--ref", type=float, default=R828D_REF_HZ,
                    help=f"R828D reference via Si5351 CLKB, Hz (default {R828D_REF_HZ})")
    ap.add_argument("--bias-tee", action="store_true",
                    help="enable VHF-port DC bias")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="HF GPIO control word (hex); default: read live from GETSTATS")
    ap.add_argument("--force", action="store_true",
                    help="proceed despite CLKB-off / ID-mismatch (debug)")
    ap.add_argument("--load", metavar="IMG",
                    help="load firmware if in bootloader (via fx3_cmd load IMG)")
    ap.add_argument("--fx3-cmd", default="fx3_cmd",
                    help="fx3_cmd binary for --load")
    return ap.parse_args()


def main():
    args = parse_args()
    if usb is None:
        sys.exit("pyusb is required: pip install pyusb")
    if args.load:
        firmware_load(args.load, args.fx3_cmd)
    app = VHFRadioApp(
        freq_hz=int(args.freq),
        ref_hz=int(args.ref),
        bias_tee=args.bias_tee,
        base=args.base,
        force=args.force,
    )
    app.run()


if __name__ == "__main__":
    main()
