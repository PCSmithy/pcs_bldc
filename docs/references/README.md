# References

Papers backing the project's control design. Large PDFs stay out of the
repo where possible — cite title + DOI here instead.

## Future controls references

- Prabhu, N., Rajaram, T., Ashok, B., "Design of sliding mode model
  predictive dual-loop control through self-learning strategy to mitigate
  the torque ripple in BLDC motor for electric vehicles."
  Scientific Reports 15, 45617 (2025).
  https://doi.org/10.1038/s41598-025-29445-w

## In-repo

- `rotor-observer-derivation.md` (+ `.pdf`) — the rotor
  estimator derived end to end: plant, state-space form, Luenberger
  observer and error dynamics, pole placement for l₁..l₃, and the
  prediction and correction discretisations. Supplement to
  `fw~est_rotor_001~1` / `fw~est_rotor_002~1`. The PDF is rendered
  from the Markdown; edit the `.md` and regenerate with
  `.venv/Scripts/python tools/md_to_pdf.py docs/references/rotor-observer-derivation.md
  --eyebrow "pcs_bldc | Supplement to fw~est_rotor_001~1 and fw~est_rotor_002~1"`.
- `actuators-14-00400-v2.pdf` — Abouseda, A.I., Doruk, R., Emin, A.,
  Akdeniz, O., "Modeling, Dynamic Characterization, and Performance
  Analysis of a 2.2 kW BLDC Motor Under Fixed Load Torque Levels and
  Variable Speed Inputs: An Experimental Study." Actuators 14, 400 (2025).
  https://doi.org/10.3390/act14080400
