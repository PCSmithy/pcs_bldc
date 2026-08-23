---
status: draft
tags: [app, views]
---

# Value rendering

### Value rendering
`app~views_013~1`

The app shall render a signal's displayed values per the value's
scalar type:

| Value | Rendered as |
|-------|-------------|
| A boolean | true or false |
| An integer | The whole number |
| An enumeration value with a resolved enumerator name (`app~obs_001~1`) | The enumerator name |
| An enumeration value with no resolved name | The whole number |
| A float of magnitude below 1000 | Four decimal places |
| A float of magnitude 1000 and above | One decimal place |

Acceptance:

- A value of each table row renders as that row states in every
  surface displaying it.

See also: [[table]], [[cursor]], [[watch-panel]]

Covers:
- sys~arch_002~1

Needs: impl, test
