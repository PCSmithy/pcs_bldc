---
status: draft
tags: [app, views]
---

# Log view

### Log view
`app~views_003~1`

The app shall display received `LogText` content as a text log in
emission order, split into lines at newline boundaries.

Acceptance:

- Text arriving split across multiple `LogText` messages renders
  complete and in order, one rendered line per newline-terminated
  line of firmware output.

Covers:
- sys~obs_007~1

Needs: impl, test
