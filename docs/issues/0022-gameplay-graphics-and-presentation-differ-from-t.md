---
id: 22
title: Gameplay graphics and presentation differ from the static reference
status: investigating
symptom: User observes horizontal and vertical lines, frozen Merry Men, missing cell doors, enemies and items, and soft/bilinear-looking presentation.
state_items: S005,S023
tags: render,oracle,interpreter
created: 2026-09-12
updated: 2026-09-12
---

Use matched game speed, renderer, width, input timeline, and frame anchor for every oracle comparison. The earlier frozen GET READY capture at frame 480 was a no-pace/turbo configuration mismatch (issue 0021), not proof of this defect. At normal speed, level-1 frame 480 reaches gameplay in both products; deeper representative frames and levels are still needed to localize missing objects, doors, animation, lines, and final presentation scaling. Vulkan blit and atlas samplers are already configured nearest, so measure the actual display path/HiDPI output before changing filters. The static recomp is the only behavioral oracle; do not use a diagnostic emulator.
