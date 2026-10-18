# Battery Power Supply Components (LDO + LiPo Charging/Protection)

Component picks for running SynthPass hardware off battery power, covering both a
CR2032 coin cell and a small rechargeable LiPo, plus USB charging for the LiPo case.
All parts are sourced from LCSC with current (checked via web search) stock/pricing.
Prices/stock fluctuate — re-check at BOM-lock time.

Context: CH572 has no ADC and its "5V supply" power scheme disables Sleep/Halt/Shutdown
modes entirely, so the whole design must run off the "single 3.3V supply" scheme
(power VDD33/VIO33 directly, V5 floated or shorted to VDD33) to keep the low-power
modes the battery-life estimates elsewhere in `docs/` depend on.

---

## 1. LDO (regulator between battery and VDD33)

**Recommendation: Torex XC6206P302MR-G — LCSC [C9972](https://www.lcsc.com/product-detail/C9972.html)**

| Spec | Value |
|---|---|
| Output | 3.0V fixed |
| Quiescent current | 1.0 µA typical |
| Input voltage range | 1.8V – 6.0V |
| Dropout voltage | 250 mV @ 100mA (3.0V type) — real dropout at this design's actual ~10–20mA loads should be well under that, but wasn't confirmed against the full current-vs-dropout curve |
| Max output current | 200 mA |
| Package | SOT-23-3L |
| Price / stock (LCSC) | ~$0.062/unit, 4,000+ in stock |

**Why 3.0V output, and why this part specifically:**

- VDD33 accepts 2.0V–3.6V (absolute max ~4.0V). A 3.0V regulated output sits close to
  CR2032's own nominal voltage (minimizing time spent "in dropout" when running off a
  coin cell) while keeping ~600mV of margin below VDD33's ceiling for a LiPo's 4.2V
  full-charge input.
- The 1.8V minimum input is the deciding spec versus the other obvious candidate,
  Microchip's MCP1700 (2.3V–6.0V input range, 1.6µA Iq, also cheap and in stock —
  LCSC [C39051](https://www.lcsc.com/product-detail/Linear-Voltage-Regulators-LDO_Microchip-Tech-MCP1700T-3302E-TT_C39051.html)).
  A CR2032 sagging toward end-of-life under a TX-current pulse (~20mA) can realistically
  dip to or below 2.3V — right where MCP1700's input floor would leave it, and where
  the XC6206's extra 500mV of headroom actually matters. XC6206 is also the cheaper
  part of the two.
- 1.0µA Iq is negligible against every duty-cycled power scenario worked out elsewhere
  in `docs/` (all ≥0.26mA average), though it's roughly comparable to the CH572's own
  Sleep-mode current (0.46–1.2µA) — worth knowing if a design ever pushes toward
  multi-second poll intervals where total system current approaches single-digit µA.

**Caveat:** dropout and Iq-vs-load-current curves weren't confirmed from the full
datasheet body (only the summary electrical table) — worth a final check against
Torex's full datasheet before locking the BOM.

---

## 2. LiPo charging + protection (not applicable to the CR2032 build)

Two things need covering, and they're not the same thing:
1. **Charging** — safely constant-current/constant-voltage charge the cell from USB.
2. **Protection** — prevent over-discharge / over-current damage during normal
   (non-charging) operation. A charger IC only guards the charging path; it does
   nothing while the device is running off the battery and not plugged in.

### 2.1 Charger IC

**Recommendation: TP4057 — LCSC (multiple manufacturers/variants stocked, e.g. UMW
[C725791](https://www.lcsc.com/product-detail/C725791.html), TPOWER
[C440390](https://www.lcsc.com/product-detail/C440390.html))**

| Spec | Value |
|---|---|
| Topology | Single-cell Li-ion linear CC/CV charger |
| Charge voltage | Fixed 4.24V |
| Charge current | Set by one external resistor, up to typical linear-charger range |
| Reverse-charge protection | Built in — no external blocking diode needed |
| Input-disconnected leakage | <1µA (auto sleep mode) |
| Package | SOT-23-6 |
| Price / stock (LCSC) | Sub-$0.05/unit on several variants, hundreds of thousands of units in stock |

Same minimal-passive-count application circuit as the more well-known TP4056/MCP73831
(one ISET resistor + input/output caps), but cheaper and with reverse-charge
protection built in for free, which is why it's the pick over:
- **MCP73831** (Microchip, LCSC [C424093](https://www.lcsc.com/product-detail/Battery-Management-ICs_Microchip-Tech-MCP73831T-2ACI-OT_C424093.html),
  ~$0.39–0.50/unit) — the more "established/major-brand" option if traceability or
  Microchip's documentation ecosystem matters more than minimizing cost, otherwise
  strictly more expensive for equivalent passive count.
- **TP4056** (LCSC C382139 and others, SOP-8 w/ thermal pad, from ~$0.03–0.07/unit) —
  the ubiquitous hobbyist-market default, but SOP-8 with an exposed pad is a slightly
  less convenient footprint than TP4057's SOT-23-6, and TP4056 lacks TP4057's built-in
  reverse-charge protection.

**What TP4057's "protection" does *not* cover:** it only protects the charging path
(reverse current, its own overcharge cutoff at 4.24V). It does nothing to prevent
over-discharge or over-current while the device is running off battery power and not
connected to USB.

### 2.2 Discharge protection (over-discharge / over-current)

**Recommendation: DW01A + FS8205A pair** — the standard, near-universal combination
for this (DW01A is the protection controller; FS8205A is the dual back-to-back MOSFET
it drives to cut off charge/discharge paths).

- DW01A — LCSC e.g. [C351410](https://www.lcsc.com/product-detail/C351410.html)
  (PUOLOP), from ~$0.014–0.024/unit.
- FS8205A — LCSC e.g. [C16052](https://www.lcsc.com/product-detail/MOSFETs_Fortune-Semicon_C16052.html),
  from ~$0.02–0.11/unit depending on manufacturer/variant.

Combined cost is a few cents, SOT-23-6 packages for both, and this is the same pairing
used in essentially every commercial small-LiPo protection circuit — well-proven, no
exotic passives beyond the pair itself.

**Before adding this to the BOM: check whether the battery you're sourcing already
includes it.** Small pre-wired LiPo cells sold to hobbyists very commonly ship with a
protection PCB built into the battery assembly itself. If so, a second protection
stage on the SynthPass PCB is redundant — check the specific battery/vendor before
deciding whether `2.2` is needed at all. If sourcing bare/unprotected cells, it's
necessary.

---

## Summary BOM (LiPo variant)

| Function | Part | LCSC | Approx. price |
|---|---|---|---|
| VDD33 regulator | XC6206P302MR-G | C9972 | ~$0.06 |
| USB charger | TP4057 | C725791 / C440390 | ~$0.03–0.05 |
| Discharge protection *(only if cell isn't pre-protected)* | DW01A + FS8205A | C351410 + C16052 | ~$0.03–0.13 combined |

## Summary BOM (CR2032 variant)

| Function | Part | LCSC | Approx. price |
|---|---|---|---|
| VDD33 regulator | XC6206P302MR-G | C9972 | ~$0.06 |
| Charging / protection | none — primary cell, not rechargeable | — | — |
