# Zoom & readability — rendering for comfortable reading without horizontal scroll (design note, 2026-06-30)

**Problem.** The panel is 1620 px wide. If WebKit lays the page out at **1620 CSS px** (device-scale-factor
1.0 — our likely current default), desktop sites render a wide layout with **small text**, and wide
content (tables, images, fixed-width layouts) forces **horizontal scrolling** — uncomfortable on e-ink.

**Levers (use the right one per case — none of these "break" the page; they use the page's own
responsive layout, or replace it):**

1. **Device scale factor (DPR) — the key default lever.** The rMPP is ~227 DPI (≈2.3×). Set the web
   view's **device scale factor to ~2.0** → the CSS layout viewport becomes ≈ **810 px** → responsive
   sites render their **tablet/narrow layout**, text is ~2× larger, and content **reflows to fit** (no
   horizontal scroll). Biggest readability win, essentially free. (cog exposes this as `--device-scale`;
   in WPE it's the view/toplevel scale — verify the exact API: `wpe_view`/toplevel scale or a
   `WebKitSettings`/`WebKitWebsitePolicies` knob.) **We almost certainly render at DPR 1.0 now — set
   this first; it likely fixes 80% of the readability problem on its own.**
2. **Page zoom** (`webkit_web_view_set_zoom_level`) — the user "bigger/smaller text" control. It pairs
   WITH the viewport: on responsive content it reflows nicely; on a fixed-width desktop layout, page
   zoom **alone** just scales and **causes horizontal scroll** — so it's a secondary control, not the
   primary fit lever.
3. **Reader mode (Readability.js)** — THE comfortable-reading answer for articles: strips the site
   layout and **reflows to a single clean column** at a controlled width → guaranteed no horizontal
   scroll, plus full font/width/line-spacing control. (Already the marquee feature in the MVP spec §5.)
4. **CSS clamp injection** for general (non-reader) browsing: inject a user stylesheet
   `img,video,table,pre{max-width:100%!important;height:auto} html{overflow-x:hidden}` so wide media/
   tables can't force horizontal scroll. Cheap; smooths over non-responsive sites.
5. **Vertical pagination (already done):** a page turn = scroll one viewport height; we **never**
   horizontal-scroll. Combined with the levers above = paged, fits-width reading.

**Recommended approach:**
- **Default:** device-scale-factor ≈ 2.0 (readable + responsive reflow + fits width) + the CSS clamp (#4).
- **User control:** discrete page-zoom steps (text size) via `set_zoom_level`, persisted.
- **Articles:** reader mode (#3) — the clean reflow, the primary "read comfortably" path.
- **Tune:** sweep the DPR on real sites (try ~1.75 / 2.0 / 2.3) to find the rMPP sweet spot; expose it as
  a setting if needed. Reader-mode width (~60–85ch) and font size are separate, already-specced controls.

**Why this is the right framing:** "don't break the layout, no horizontal scroll" is achieved by
(a) letting the site's OWN responsive layout do the work at a sensible viewport (DPR), and (b) reader
mode replacing the layout for articles — not by trying to re-flow arbitrary desktop CSS ourselves
(which is the hard, fragile path). Page zoom is a comfort knob on top, not the fit mechanism.
