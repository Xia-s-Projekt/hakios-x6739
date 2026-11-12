# Handoff: SpringBoard Home Screen App Launch & Paging Touch Fixes on Infinix GT 10 Pro (X6739, MT6893)

**Date**: 2026-11-08  
**Target Device**: Infinix GT 10 Pro (`X6739`), MediaTek Dimensity 8050 (`MT6893`, `MT6893Z_T/CZA`), Mali-G77 MC9, ADB Serial: `XNU_SERIAL`.  
**Target OS / System**: HakiOS (Darwin / XNU 16.5) booting iOS 16.5 rootfs via APFS (`ufs_xnusys`).  
**Active Slot**: `_a`  

---

## 1. Executive Summary & Session Status

- **Status**: 
  - Comprehensive reverse-engineering of iOS 16.5 dyld shared cache completed across `SpringBoard`, `SpringBoardHome`, and `UIKitCore`.
  - Identified and eliminated the dual failure mode that broke app icon launches and horizontal page scrolling while allowing widgets and bottom edge home gestures to function.
  - Rebuilt `libhakios.sb.dylib`, generated new APFS root filesystem (`build-root.apfs`, 6,426,624 bytes), packed `build-payload.bin`, and flashed `boot_a` with `xnu-flashed-slot_a.img`.
  - Executed a 300-second on-device test run via `tools/xnu-cycle.sh`.
  - Backboardd sustained high-framerate rendering peaking at **88.3 FPS** (frame 8955, >203,000 GPU job submissions, `gpu_fails=0`).
  - Extracted post-run ramoops log (`logs/xnu-last.log`, 256,470 bytes) and decoded base64 framebuffer capture (`fbshot.png`, 160×352 RGB).

---

## 2. Root Cause Analysis

### A. Why App Icons Failed to Launch While Widgets Succeeded
1. **`SBIconView` Tap Gesture Rejection Chain**:
   - `SBIconView` manages a dedicated `UITapGestureRecognizer *_tapGestureRecognizer` (ivar offset `0x4f0`).
   - When tapped, `-[SBIconView gestureRecognizer:shouldReceiveTouch:]` calls `[delegate iconShouldAllowTap:self]`, which delegates to `-[SBIconController iconManager:reasonToDisallowTapOnIconView:]` and `-[SBIconController iconManager:reasonToDisallowInteractionOnIconView:]`.
   - If any disallow reason string is returned, SpringBoard executes `SBLogIcon(@"Not allowing tap gesture to receive touch because the delegate (%{public}@) said so.")`, terminating gesture evaluation before `-[SBIconView _handleTap]` is reached.
2. **Widgets Bypass Tap Validation**:
   - In `-[SBIconView gestureRecognizerShouldBegin:]` (PC `0x1c44e2cfc`), if `self->_customImageViewController` (`CHSWidgetHostViewController`, ivar `0x4e8`) is present and interactive, `SBIconView` returns `NO`, disabling its own `_tapGestureRecognizer`.
   - UIKit routes touches directly into the widget subviews (`UIControl` / URL handler) which dispatch directly to `SBWorkspace`, bypassing all `SBIconView` tap veto checks.
3. **Invalid Selector Swizzle in Prior Build**:
   - An earlier build attempted `safe_swizzle_inst_method("SBIconController", "iconManager:shouldAllowTapForIconView:", ...)`. This selector does not exist in iOS 16.5 and failed with `not found for swizzle`. The actual selectors are `iconManager:reasonToDisallowTapOnIconView:` and `iconManager:reasonToDisallowInteractionOnIconView:`.

### B. Why Horizontal Icon Page Paging Failed
- `SBRootFolderView` (subclass of `SBFolderView`) contains the paging `SBIconScrollView` (`_scrollView`).
- `SBFolderView` maintains `_scrollingDisabledReasons` (`NSMutableSet`). When non-empty, `_scrollView.scrollEnabled` is set to `NO` and `_scrollingDisabledGestureRecognizer` intercepts all horizontal drag gestures.
- Because lock screen / cover sheet transition assertions remained active, horizontal scrolling was blocked.

### C. Touch Shadowing & Gesture Interception
1. **`SBHomeScreenWindow` Switcher State Gating**:
   - `-[SBHomeScreenWindow hitTest:withEvent:]` returns `nil` whenever `[SBSwitcherController unlockedEnvironmentMode] != 1` or `[SBHomeScreenViewController isIconContentHidden] == YES`.
   - Falling back to standard `-[UIWindow hitTest:withEvent:]` ensures touches reach `SBIconView` and `SBIconScrollView` even if the switcher modifier state machine lags.
2. **`SBHomeScreenOverlayWindow` Interception**:
   - Sits in front of `SBHomeScreenWindow` (level 1.0–2.0). Returning `nil` for empty background container hits allows touches to reach app icons underneath.
3. **CoverSheet Gesture Freezes**:
   - `my_coversheet_window_hittest` previously checked `point.y <= 40.0`. During pull-down or drag-up gestures, as soon as the touch coordinate moved past 40.0, the hook returned `nil`, causing UIKit to cancel the active gesture and freeze the interface.
   - Fix: Full hit-testing is preserved whenever `isUILocked`, `isCoverSheetPresented`, `isTransitioning`, or `isAnyGestureActivelyRecognized` is true.
4. **`apply_wallpaper()` Interference**:
   - Previously forced `setHidden:NO` on all sublayers of `SBHomeScreenWindow.layer`, unhiding dimming curtains and blur overlays that intercepted touches.
   - Previously invoked `[w makeKeyWindow]` every 5 seconds from `battery_post_notifications()`, continuously resetting the first responder and canceling active gestures.

---

## 3. Implemented Changes

### In `refs/hakios-xnu-kernel/userland/src/inject_sb.c`:

1. **CoverSheet & Control Center Hit-Testing**:
   - `my_coversheet_window_hittest`: Evaluates `isUILocked`, `isCoverSheetPresented`, `isTransitioning`, and `isAnyGestureActivelyRecognized`. Falls through to `SBHomeScreenWindow` only when fully unlocked and dismissed.
   - `my_controlcenter_window_hittest`: Preserves hit-testing during active presentations and transitions (`isVisible`, `isPresentedOrDismissing`, `!isDismissedOrDismissing`).
2. **HomeScreen & Overlay Window Fallback**:
   - `my_homescreen_window_hittest`: Invokes `original_homescreen_window_hittest` and falls back to standard `-[UIWindow hitTest:withEvent:]` if `nil` is returned.
   - `my_overlay_window_hittest`: Filters out hits to `self` or `rootViewController.view` to pass touches to `SBHomeScreenWindow`.
3. **`SBIconView` Tap Gesture Recognizer Hooks**:
   - Hooked `-[SBIconView gestureRecognizer:shouldReceiveTouch:]` to force `YES` for `_tapGestureRecognizer`.
   - Hooked `-[SBIconView gestureRecognizerShouldBegin:]` to return `YES` for app icon taps and `NO` for interactive widgets.
   - Hooked `-[SBHIconManager reasonToDisallowTapOnIconView:]` -> returns `nil`.
   - Hooked `-[SBIconController iconManager:reasonToDisallowTapOnIconView:]` -> returns `nil`.
   - Hooked `-[SBIconController iconManager:reasonToDisallowInteractionOnIconView:]` -> returns `nil`.
   - Hooked `-[SBLeafIcon isLaunchEnabled]`, `-[SBApplicationIcon isLaunchEnabled]`, and `-[SBApplication iconAllowsLaunch:]` -> returns `YES`.
   - Hooked `-[SBHIconManager isEditing]` -> returns `NO`.
   - Hooked `-[SBIconController isHomescreenVisible]` and `isHomescreenEffectivelyVisible` -> returns `YES`.
   - Hooked `-[SBHomeScreenViewController isIconContentHidden]` -> returns `NO`.
4. **`SBFolderView` Scroll Unblocking**:
   - Hooked `-[SBFolderView isScrollingEnabled]` -> returns `YES`.
   - Hooked `-[SBFolderView _setScrollingDisabled:forReason:]` -> forces `disabled = NO`.
   - Hooked `-[SBRootFolderView didMoveToWindow]` to clear `_scrollingDisabledReasons`, detach `_scrollingDisabledGestureRecognizer`, and enable `scrollEnabled`.
   - Added `unblock_root_folder_scrolling()` utility called on init, 2s, and 5s.
5. **Wallpaper & Periodic Timer Cleanup**:
   - Removed periodic `apply_wallpaper()` calls from `battery_post_notifications()`.
   - Stripped the sublayer unhiding loop and `makeKeyWindow` call from `apply_wallpaper()`.
   - Maintained default Lock Screen presentation without automated swipe-up gestures.

---

## 4. Verification & Build Records

1. **Compilation**:
   - Rebuilt `obj/libhakios.sb.dylib` with Clang 19 cross-compiler targeting `arm64-apple-darwin` with `iPhoneOS16.5.sdk`.
   - Signed with `ldid -S obj/libhakios.sb.dylib`.
   - Deployed to `root/lib/libhakios.sb.dylib` and `prebuilt/userland/libhakios.sb.dylib`.
2. **APFS Root Generation**:
   - `tools/mk-root.sh` generated `build-root.apfs` (6,426,624 bytes).
3. **Flashing & Cycling**:
   - Target slot: `boot_a` (active slot `_a`).
   - Flashed via fastboot and booted XNU with `XNU_SECS=300`.
   - Device cycled cleanly and returned to Android.
4. **Git Repository Status**:
   - `refs/hakios-xnu-kernel`: Commit `fe24e3f` (`userland: fix springboard homescreen icon touch and folder scrolling`). Clean working tree, zero AI attribution trailers.
   - `hakios-x6739`: Clean working tree.

---

## 5. Next Steps

1. **User Acceptance Testing on Physical Device**:
   - Tap standard app icons (e.g. Settings, Camera, Safari, Phone) on the home screen grid to verify application launch.
   - Swipe horizontally across home screen icon pages to verify page paging.
   - Drag up from the bottom screen edge to verify App Switcher / Recents menu access.
   - Drag down Notification Center / Lock Screen and drag up to verify dismissal without UI freezes.
2. **Screen Tearing & VSync Optimization**:
   - Address the display timing and tearing issue noted by the user (`fb_ca_hz=120`, VSync synchronization between Panfrost/DRM and CoreAnimation display links).
