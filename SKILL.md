---
name: allocsight-disk-cleanup
description: High-speed Windows disk space inspection and intelligent cleanup analysis using AllocSight CLI (allocsight.exe). Use when the user asks to check disk space, analyze what is occupying C:/D:/E:/F: drives, find caches/duplicate archives/large files, or generate a clickable Markdown cleanup report.
---

# AllocSight Disk Space & Cleanup Skill

Use `allocsight.exe` to perform sub-second, cluster-accurate disk space analysis on Windows and generate structured, clickable cleanup recommendations without modifying user files unless explicitly instructed.

## Core Commands

1. **Overview of all mounted drives**:
   ```powershell
   .\allocsight.exe drives
   ```
2. **Hierarchical allocation tree of a drive or directory**:
   ```powershell
   .\allocsight.exe tree C:\ -d 3 -m 300mb -n 20 -q
   ```
3. **Smart cleanup diagnostic (SAFE caches vs. REVIEW archives/artifacts)**:
   ```powershell
   .\allocsight.exe analyze F:\ -n 40 -q
   ```
4. **Generate a clickable Markdown report (`file:///` links)**:
   ```powershell
   .\allocsight.exe report D:\ -n 40 -o cleanup_report.md -q
   ```

## Best Practices for Automated Space Cleanup

1. **Read-Only Inspection First**: Never delete user files during inspection. Always present findings grouped by safety tier (`[SAFE] Caches & Temporary Data`, `[REVIEW] Redundant Archives / Duplicate Directories / Build Artifacts`, `[CAUTION] User Data`).
2. **Verify Ambiguous `temp` or `cache` Folders**: Before recommending whole-folder deletion of a user-created `temp` or `cache` folder, drill down (`.\allocsight.exe tree "<folder>" -d 3 -q`) to verify it does not contain source code, model weights, or active databases.
3. **Always Provide Clickable `file:///` Links**: Format every folder and file using percent-encoded `file:///` URIs (`%20` for spaces) so the user can jump directly to any location from the report.
