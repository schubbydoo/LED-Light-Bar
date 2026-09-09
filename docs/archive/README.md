# archive/

Superseded work, kept because the reasoning still has value even though the decisions changed.

- **`salvaged-fog-panel.md`** — the original project. A 6-emitter RGB panel salvaged from a fog
  machine, and the mains driver that came with it. Abandoned not for power (3–9 W is normal for
  that class of fixture) but for **voltage**: three dies in series puts blue and green at ~9.6 V,
  which forces a boost stage off any battery and browns out before a 3S pack is flat.
  Still useful for: the common-anode analysis, why PT4115-class buck drivers cannot drive a
  common-anode panel, and the TLC5947 / MOSFET options if that panel is ever used on a wall wart.

- **`bench-test-procedure.md`** — how to characterise an unknown LED panel with only a 12 V
  battery, a DMM and resistors. Written because there was no bench supply. Moot for this build,
  genuinely reusable for the next salvaged part.
