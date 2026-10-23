# Surfaces — UI design language

**Frozen baseline.** Changes to these principles or components require explicit approval.

The freeze is an approval policy, not an automated file lock. Proposed changes must identify the affected rules,
profiles, and component states, and receive explicit approval before this specification is revised.

Surfaces is an application-independent UI design language. It defines how content, controls, and their
containers form a coherent interface through shared geometry, hierarchy, states, and motion. It is
not a color theme, a rendering technique, or a design specific to one application or toolkit.

The principles stand on their own. Product identities and technology bindings belong in separate documentation.
Approved platform or product exceptions belong in named profiles in §10; each overrides only the rules it lists.
The qvim reference palette is authoritative in §11. The
[qvim application guide](implementation.md) maps these rules to the approved screens and implementation targets.

## 1. The core idea

A **surface** is a visual area that provides a background and context for content or interaction. It
is a design concept, independent of which application renders it or how it is rendered.

There are three **primary surface roles**: the **frame** around the workspace, a **pane** for each body
of content, and **overlays** above their hosts while needed. These are roles, not a limit of three
depth levels or three surfaces. Components such as chips, cards, fields, and buttons can have their
own nested surfaces. A menu has an overlay surface; a submenu is another overlay, not another role.
Nesting and interactivity do not, by themselves, imply elevation.

Each surface takes its color from its source — the content's theme, with the implementation's palette
as fallback — not its position in a stack. Separation expresses purpose: a gap between frame and pane,
local tones for component states, and a soft shadow for an overlay.

The ideology in one line: **content is sovereign; chrome recedes; elevation serves a purpose.**

Five beliefs follow from that, and every design decision must be traceable to one of them:

1. **Content is sovereign.** The material being read, created, or explored — text, images, data —
   owns the screen and owns the color. Chrome derives its colors from the content's own theme and
   never competes with it.
2. **Surface roles, ordered by purpose.** Frame, pane, and overlay organize the workspace; nested
   component surfaces organize what is on them. A new background does not automatically create a new
   elevation. Shadow is reserved for overlays that genuinely float above their hosts.
3. **Recede, don't compete.** Every persistent pixel of chrome is subtracted from content. Chrome is
   compact, quiet, and where possible revealed on demand rather than always present.
4. **One geometry everywhere.** The same surface roles, radius, gap, and motion tokens apply across
   components and products. A user who learns one surface has learned them all.
5. **Calm by default.** Motion is minimal and purposeful; translucency is tasteful and never harms
   legibility or performance. Accessibility floors are a starting point, not a target.

## 2. Surface roles and nesting

**Chrome** is the interface surrounding and supporting content: the frame, bands, controls, and their
backgrounds. A **component** is a unit of layout or interaction; its **surface** is the visual area
that hosts it. A component may contain other components and their surfaces.
Component types and their behavior are defined in §9; they are not additional surface roles.

The primary roles are:

- **Frame** — the application view's surrounding chrome: its title or navigation area and the margin
  around the panes, one continuous sheet. It carries view-level controls rather than the main content.
  A pane's header may ride in the frame above the pane rather than cost it a band of its own. On an
  opaque view the frame is one tone step darker than the pane it holds; with a platform backdrop
  material it takes that material (see Translucency).
- **Pane** — one per body of content: a document, a collection, a data view. It rests on the
  frame, inset by the gap, and takes the content's own background. Everything else that belongs to its
  content lives on it: its tab band, regions, status line, scrollbars, and other controls. A translucent
  pane is an explicit opt-in only.
- **Overlay** — a surface that appears above its host while needed: a menu, popup, hover, or notification.
  Its host can be the frame, a pane, or another overlay. It takes
  the content's overlay color, so it may be lighter or darker than its host. A soft shadow, and with
  backdrop materials a blur of what is behind it, expresses elevation — never a prescribed tone.
  Within the interface, only overlays cast shadows; native window decoration is platform-owned (§4).

Use **one frame per application view, one pane per body of content, and overlays only while needed**.
An overlay stack may have several levels, each above its host. Nested component surfaces do not add
workspace levels: use them for grouping or interaction, not to decorate every container.

Within those roles:

- **Bands** — strips of chrome along a host's edge: a toolbar, tab band, or card header. No fill of
  their own; they share the host's surface and hold elements.
- **Regions** — subdivisions of a pane, such as split views. A region is marked by a soft outline in the
  raised tone (§3), with a small gap between neighbors. Its outline uses the accent when the region
  itself is an active text-editing area, not because a control inside it is highlighted.
  A region shares the pane's background rather than introducing a separate fill.
- **Elements and nested surfaces** — chips, cards, fields, buttons, indicators, and cursors,
  built from §9's primitives. A component's surface takes its color from its host, stepped by state
  (§3), or uses an accent or status color where permitted. A card can host chips and fields without
  becoming a pane or an overlay. An element that locally covers content — an overview or sticky summary —
  borrows the overlay's color, without its shadow; local overlap alone does not create overlay elevation.
  A scrollbar shows only its thumb at rest; its track and markers appear during interaction.
- **Content** — the material being read or edited: text, images, documents, data, and their meaningful
  formatting. The language specifies its relationship to the surrounding interface, not which
  application or rendering system produces it.

## 3. Color and separation

Depth follows the host and overlay relationships; tone does not encode it. Each surface takes its
color from its source, and separation expresses its role. Concrete hex values live in an
implementation's palette or content theme; qvim's palette is in §11. These are the shared defaults,
subject only to the explicit profiles in §10:

- **Pane — the content's background.** The content theme is the source of truth: background, text,
  syntax, cursor, and text-range selection come from it where defined. Chrome text is the content's
  text color too. Selecting a control uses the component-selection treatment below.
- **Frame — one tone step darker than the pane,** in both themes; or the OS material.
- **Overlay — the content's overlay colors**, with a dedicated menu color where the theme defines one.
- **Elements — a step from the surface under them, by state.** A step is one move along the palette's
  tones — frame, resting chip, raised chip, window edge — carried to whatever background the content
  sets: toward the text color from a pane to its resting chip, then its raised chip; the other way to its
  frame. At rest a chip is one step from its surface and a menu item none; unselected hover, open,
  and pressed states are two — the raised tone. Selection is a separate accent tint, not another
  neutral step. The resting-chip role is derived once from the canonical pane and stored as a fixed
  theme token; it does not change with the active content pane. Other element steps are about 4 % of
  the way to the text color; as a tint, exactly 4 % (below).
- **Status — semantic colors** for error, warning, information, hint, success, and added, changed, or
  removed content. Error — including failure — warning, and success use the implementation's shared
  palette across editor, chat, and app. Information, hint, and content-change colors use the content
  theme where defined.
- **Accent — the implementation's designated accent**, unless the user overrides it. Surfaces defines
  its uses, not a particular hue.

For roles not fixed above, where the content defines no color, it falls back to the implementation's
palette; the resolution order is explicit override → content-derived → palette fallback. The cursor
falls back to the text color, reversed — never pure white or black. Every surface, step and state is
defined for light and dark together; light is not an afterthought.

Four rules keep roles and states distinct:

- **Frame and pane: the gap and one tone step.** The frame shows as a thin margin around each pane.
- **Host and overlay: a shadow, never tone.** An overlay may share its host's tone or contrast with it,
  as the content's colors dictate; with backdrop materials it also blurs what is behind it.
- **Regions: a soft outline and a small gap,** in the raised tone. Outlines express subdivision or
  active text editing, not elevation. Do not border every surface to separate it; platform window edges are
  a separate responsibility (§4).
- **Element states: tone and selection tint.** Unselected hover, open, and pressed states use neutral
  tones; selected controls use an accent-tinted background. An active text-editing area has an accent
  border. None of these states creates elevation or a shadow.

These keep chrome quiet on a surface:

- **Hairlines split the parts of one thing.** A hairline — the text color at 12 % — may divide the parts
  of one overlay or element: a menu's groups, a table's header, or a sequence's sections. A card's
  rounded body is separated from its heading by its inset, not a hairline.
- **Cards have an outer surface and an inset body.** The heading shares the outer surface. A soft
  card's body uses the content background; its outer color uses the named soft-outer tone. A status
  card's outer surface uses its status tint; its inset body uses the named status-body tone. Closely
  spaced body tones may alias a shared palette value when the status icon, label, and outer surface
  preserve the meaning. Neither surface adds a shadow.
- **Active text editing has an accent border.** Use it only where a cursor can be placed and text
  edited, while that area is active. Inactive and read-only fields have no editing border. Buttons,
  tabs, and menu items are not editing areas; do not add borders to suggest keyboard navigation.
- **Selection is an accent-tinted background.** A selected tab, choice chip, or menu item uses
  the named selection tone and the selected-hover tone under the pointer. The two roles may alias in a
  theme where their visual difference is negligible. No additional selection icon is required.
  Opening a menu or pressing an ordinary action does not by itself select that control. This rule
  does not tint every pane or card, or replace the content theme's text-range selection.
- **The accent is reserved** for active text editing, component selection, the scrollbar thumb, and
  work in progress. Content never takes it merely because it is interactive: links and
  references use the content theme's own semantic styles.
- **Status colors mean status** and nothing else, save one: a menu's destructive action takes the error
  color, last in the menu, under a hairline.
- **Icons keep their own colors.** An icon whose color names what it is, not a state — file-type icons,
  provider marks, category icons, or an application's identity — keeps that color.
- **Buttons share one treatment.** Action buttons use a neutral tone from their host and the normal
  text color. No action receives a special fill because it is primary, preferred, or stops work.
  Hover, pressed, and disabled appearances come from the shared state rules, not a priority ranking.
- **Text has two levels.** The text color for primary content; one fixed opaque muted tone for secondary
  and inactive content — metadata, resting tabs, placeholders, disabled controls, and dimmed prefixes.
  The muted tone is chosen per theme to remain legible across supported chrome surfaces and does not
  change with its host layer. There is no separate faint level.
- **Tints come in steps of 4 %,** of the text, status, or accent color: 4 and 8 % for an element's two
  steps where they are tints, and 8 % for an indicator's band (a track or viewport marker); 12 % — a
  hairline's — for a status fill (an amber card, a status chip) or a bar's track; 16 % for the strong
  step: a fill under the pointer, an open status chip, a category chip. These formulas define semantic
  roles; an implementation may alias perceptually adjacent derived roles to one named palette value.
  Nothing in between is introduced.
- **Disabled is muted, not gone.** A disabled or ended element keeps its place and shape, drops to its
  rest tone with no accent or emphasis fill, and its text and glyphs use the same muted tone as secondary content.

## 4. Metrics

These are the shared tokens. Two kinds of size, and they never mix: **geometry follows the display,
text follows the font.**

**Geometry** — corners, gaps, paddings, shadows — is in **logical pixels that scale with display/DPI,
never with content font size.** Geometry is not typography: a user raising their font does not expect
rounder corners. The default is the compact density:

- **Corner radius — primary surface: 6px.** Panes and overlays.
- **Corner radius — nested component: 4px.** Tabs, chips, menu items, cards, fields, and
  region outlines. The cursor's corners: 2px.
- **Frame gap: 4px**, the same as the paddings. The frame left visible around and between the panes on
  it. Equal on all sides so a lone pane reads as intentionally floated, not clipped. Keep it tight: with
  one tone step, the gap is the whole separation between frame and pane, so it must be visible, but every
  extra pixel is content lost.
- **Padding: 4px.** Inside a band or chip, and between a region's outline and its text. A card's heading
  uses 4px; its inset body uses 2px padding inside a 2px gap, keeping the same text alignment.
- **Nested gap: 2px.** Between a surface's edge and a region's outline on it, between two regions'
  outlines, between an overlay's edge and its rows.
- **Card body:** 2px from the outer card at the sides, bottom, and below the heading; 2px corners
  inside the card's 4px corners. The gap is the outer surface showing through, not a drawn border.
- **Overlay padding: 2px** around rounded rows, so their 4px corners are concentric with the overlay's
  6px; **4px** around unrounded text or content-owned cells, whose internal geometry stays unchanged.
  Hover uses the compact single-row metrics in §9.
- **Lead gap: 6px** between a mark that leads a line and its text: an identity icon and title, a
  status mark and label, a category chip and input.
- **Marks** — the lines, lanes and dots of indicators — reuse the sizes: a line 2px, 4px under the
  pointer; its lane 8px, the 4px line with 2px either side; a lane of marks in a map 4px; a dot 4px. A
  map draws 1px per column and 2px per line.
- **Shadow: overlays only.** Soft and diffuse, to signal "above." Never on the frame or a pane, never
  behind scrolling content.
- **Radius hierarchy:** nested component ≤ host surface. Inner corners never round harder than what
  contains them; the platform owns the outer window's shape.
- **Content-owned layout wins inside a pane.** When a content system fixes a grid or separator size,
  fit the surrounding geometry without distorting that content. Keep the 4px text padding and at least
  2px between outlines; allocate separator slack and unused edge space consistently. Font-based
  content grids can therefore affect the remaining gap, not the corner or padding tokens.

A density changes all of these together, as one coherent step (the roomier set is radius 8, inner 6,
gap 5); individual metrics are never tuned in isolation. The compact set needs only three sizes — 2, 4
and 6px — and 8 for a lane (2 + 4 + 2).

**Text** sets every size that holds it. A shared row follows the chosen body font's metrics, or the
content's cell height in a grid-based interface. Chrome and content keep one rhythm and grow together.
With a reference row of 25px:

- **Text row: 25px** — a tab, a chip, a menu or overlay row, a button, a field, a line of body text.
- **Chip: one row,** with 4px padding, 4px corners, 4px apart.
- **Band: one row with 4px above and below (33px)** — the tab band, a card's header.
- **Text inset: 6px** from a pane's edge — the outline 2px in, the text 4px inside it — so text starts on
  the same column in every region and content group.
- **One UI typeface,** selected by the implementation and shared with its body text, at the same size.
  Interface text is regular, bold for a title (a card's, a heading, a table's header), and italic for
  an aside or reflective text. Content-owned typography keeps its own styles; the language does not impose
  a particular font family or font-file format.
- **Icons follow the text metrics.** Use a consistent icon family and optical size. Inline icons occupy
  a text-aligned slot; glyph-based implementations reserve a cell and accommodate ink that extends
  beyond it. Alone in a square chip an icon is 6/7 of the text's size, clear of the chip's padding.
  Application and status artwork can use dedicated sizes in an implementation profile.
- **Overflow:** text cut at the end of its row ends in "…"; content that scrolls sideways fades out over
  its last 4 character widths (cells in a grid), the accent thumb under it showing how much is in view.
- **Content spacing:** related content parts sit 16px apart and major groups 40px; every line still
  sits on a text row. Content-owned document layout is not overridden.

Native title bars, caption buttons, window edges, and outer corners follow the host platform rather
than the component tokens. Exact platform metrics belong in separate implementation documentation.

## 5. Translucency

Translucency is an accent that signals "this is the frame or a transient overlay," and a way to tie the
window to the OS material — never a decoration applied to content.

- **Where it lives:** the frame and overlays. They sample what is behind them (the OS desktop for the
  frame; the workspace for overlays).
- **The material takes the content's light or dark,** even when the host platform's theme differs:
  chrome text is the content's text color, so the material must support its contrast.
- **Where it never lives by default:** behind the scrolling content of a pane. A translucent pane is
  available as an explicit, off-by-default opt-in only, because it costs per-frame compositing and can
  hurt legibility.
- **Background opacity does not dim content.** Changing a background's transparency leaves foreground
  text and icons unchanged; their own text-level and disabled-state rules still apply.
- **Component and state colors remain opaque.** Chips, buttons, selections, scrollbar tracks, status
  fills, and other nested UI surfaces keep their fixed or host-derived opaque colors on a material.
  Translucency belongs to the pane or overlay material, not to each component drawn on it.
- **Intensity is content-driven where the content provides it,** reinterpreted as material intensity
  rather than a flat wash. On a backdrop material an overlay keeps 72 % of its color over a 14px blur
  by default; a translucent pane keeps 40 %. Where the content supplies transparency settings, an
  implementation profile specifies their mapping. Without the backdrop treatment, surfaces are opaque.

## 6. Animation

Motion is minimal. An animation exists only to show that something changed, or that
work is under way — never for decoration. Almost nothing on screen moves.

- **Almost nothing moves.** Overlays appear and leave at once. Movement follows direct manipulation
  or reveals obscured content, rather than decorating a state change. Content motion is opt-in;
  cursor blinking follows the content or user setting.
- **Short and uniform: 120ms,** every transition the same, fast enough that it never delays a keystroke.
- **Work under way may loop** — the one motion that is not a transition: a spinner or a working-status mark.
  It runs on a small mark, never on content, changing only the mark's frames, transform or opacity, 0.8–2s
  a loop; it stops the moment the work does.
- **Never on the hot path.** An animation must never add cost to each frame of scrolling or typing.

## 7. Accessibility and performance

Two commitments the whole language keeps, independent of any one component.

- **Legible by default.** Keep comfortable contrast between text and its surface, between the frame and a
  pane, and between an element's states, so every surface and state reads as distinct and every glyph
  stays readable. Favor soft tones over pure black or pure white.
- **Never color alone.** Status and change indicators pair color with an icon, shape, or underline, so
  meaning survives color-vision differences and low contrast. Implementation-specific departures must
  be recorded explicitly (§8); they do not weaken the general rule.
- **Respect the user's settings.** Honor input and accessibility preferences; reduced-motion
  preferences are honored where motion exists — a looping mark holds still.
- **The content render path is the budget.** Translucency, shadow, and blur stay on the frame and
  overlays; effects behind scrolling content are avoided. "Pretty" must never cost the frame rate.

## 8. Applying Surfaces to a new component

When designing a new component or screen, answer in order:

1. **What is its role and host?** Is it a frame, pane, or overlay, or a component within one? Identify
   any nested surface and its host separately from its drawing order.
2. **Does a separate background or elevation serve a purpose?** A chip may need a surface for its
   state without floating; a menu may need elevation above its anchor. Reuse §9's primitives rather
   than introducing a new workspace level for every container.
3. **Apply the shared metrics** — radius, gap, padding, shadow rule, radius hierarchy, and the text
   sizes that follow the body font or content grid — unchanged, unless an applicable §10 profile
   explicitly replaces a metric.
4. **Take color from its source** — the content theme, stepped by state for elements — with the palette
   as fallback; define light and dark together.
5. **Check it against the five beliefs and the accessibility and performance commitments.** If it
   competes with content, separates surfaces with a border, leans on shadow for anything but an overlay,
   or animates content by default, reconsider.

A component is "on-language" when every one of its choices traces back to a belief in section 1 and
uses the tokens in section 4 or an applicable §10 profile. Record product-specific mappings in that
implementation's own documentation. A departure needs explicit approval and a scoped rule in §10 before it belongs to
the frozen specification; an implementation guide cannot silently override this document. Profiles are not permissions
for other implementations.

## 9. Components

Components are defined by **purpose and interaction**, not just appearance. A tab can use a chip-shaped
surface, but it selects a view; a chip represents an item or a choice. Menus use overlay surfaces,
while cards group content within a host.

The visual building blocks below compose into the component types in §9.3. A new component does not
automatically require a new primitive or surface role. Reuse the shared tokens and states; introduce
another primitive only when an existing composition cannot express the need.

### 9.1. Shared visual building blocks

Visual states, where meaningful: **rest · hover · open · pressed · selected · editing · disabled**.
In §3, +1 and +2 are neutral tone steps; selection uses an accent tint. Only active text-editing
areas receive the accent border. These examples do not assume keyboard-navigation states.

| Primitive | Role or host | Anatomy | Metrics (px; a row follows the body font or content grid) |
|---|---|---|---|
| **Surface** | frame, pane | the application view; one pane per body of content | pane 4 in from the frame, corners 6; text 6 in |
| **Overlay** | overlay above a host | rows, content-owned cells, or a compact label | corners 6; padding 2 (rows), 4 (cells); compact label metrics in §9; a menu 2 from its anchor; shadow |
| **Band** | layout within a host; shares its surface | a strip of related controls or metadata | a row with 4 above and below (33) |
| **Region** | on a pane | a 1px outline round a subdivision | corners 4; 2 from an edge or a neighbor; text 4 in — content-owned layout wins |
| **Card** | outer and nested body surfaces | heading on the outer surface; content and actions in the body | 2 in from the pane; outer corners 4; body gap, padding, and corners 2; heading padding 4 |
| **Chip** | nested component surface | icon · label · count | a row; padding 0 4; corners 4; 4 apart; square: a row each way |
| **Field** | nested component surface | text and cursor; optional attached controls | a row, padding 0 4; multiline padding 4; corners 4 |
| **Button** | on its host; local surface when filled | a chip that acts, or a glyph alone | a chip's; icon-only actions square |
| **Indicator** | element | thumb, line, bar, track, map | §4's marks; a bar 4 |
| **Divider** | on a surface | a hairline, or a label between two | 1 |
| **Glyph** | in text or a control | icon, state glyph, spinner, artwork | a text-aligned slot; 6/7 alone in a square chip; artwork sizes in the implementation profile |
| **Cursor** | element | block, bar, or underline marking an insertion or active text position | shape from content or user settings, aligned to text; corners 2 |
| **Text** | in content or chrome | body, muted, title, aside, formatted content, link, reference | the shared font size and row |

| Primitive | Rest | Hover | Open, pressed | Selected | Editing | Disabled |
|---|---|---|---|---|---|---|
| **Surface** | the content's background; the frame a step darker, or the material | | | not inherently selectable | only an actual editable area | |
| **Overlay** | the overlay color; with a backdrop 72 % over a 14px blur, or content-controlled transparency (§5) | | | not inherently selectable | only an actual editable area | |
| **Band** | no fill; a card's heading shares its outer surface | | | | | |
| **Region** | outline in the raised tone | | | on a selection control, if present | accent border if editable and active | |
| **Card** | soft: stronger outer tone, content background inside; status: same hue on both surfaces, body lightness adjusted from the outer tint (§3) | | | on its selection control | only an editable area inside it | |
| **Chip** | +1; resting in a band, its text muted | +2 unless selected | +2 unless selected | accent 12 %, 16 % hovered; text full | | |
| **Tab** | +1; resting in a band, its text muted | +2 unless selected | +2 unless selected | accent 12 %, 16 % hovered; text full | | +1, text muted; no accent |
| **Status chip** | its status color at 12 %, its text in that color | 16 % unless selected | 16 % unless selected | if a choice: selection tint plus separate status icon or label | | |
| **Field** | +1 from its content host, including an inset card body; placeholder muted | | | text-range selection is content-owned | accent border; none when read-only | text muted; no accent |
| **Button** | neutral +1; icon-only: no fill; destructive: error text | +2 unless selected | +2 unless selected | choice or toggle only: accent 12 %, 16 % hovered | | rest tone, glyph muted; no accent |
| **Indicator** | thumb the accent; a fill muted on a 12 % track; a viewport band 8 % | lane track 8 %, thumb 4px, markers; viewport band 16 % | | | | |
| **Divider** | the text at 12 %; a label muted | | | | | |
| **Glyph** | an icon its own color; a state glyph its status; a spinner the accent; a kind's icon muted | | | owned by its control | | muted |
| **Cursor** | the content's cursor color (§3) | | | | | |
| **Text** | text; fixed muted; title bold; aside italic | | | content-owned | | muted |

- **Text and icons.** Verbs, metadata, and counts muted; content-owned formatting keeps its semantic
  colors; an inline content fragment can use the +1 tone with 4px corners; icons as in §4.
- **Motion.** An indicator follows the pointer (120ms); a glyph may loop while work runs (§6). Every
  other primitive changes state at once.

### 9.2. Shared component rules

- **Semantics follow purpose.** Actions are buttons, destinations are links, choices expose their
  selected state, and tabs identify their associated panels. Appearance never substitutes for a role.
- **Every control has a name.** Prefer a visible label; icon-only controls also expose an accessible
  name. A placeholder does not replace a label.
- **Availability is explicit.** Disabled controls cannot activate; read-only content can still be read
  and selected. Busy, selected, expanded, and invalid states have meaning beyond a color change.
- **Input behavior belongs to the implementation.** Keyboard navigation, focus transfer, key bindings,
  and activation or dismissal commands are not defined by this design language.
- **No new styling per component.** The catalogue inherits geometry and text metrics from §4, states
  from §3, motion from §6, and accessibility from §7. Product-specific commands and shortcuts belong
  in implementation documentation.

### 9.3. Component catalogue

#### Band and toolbar

- **Purpose and anatomy:** a band arranges related controls or metadata in a strip at a host's edge:
  a header, tab strip, action strip, or status strip. A toolbar is a band specifically containing
  actions, optionally split into labeled groups.
- **Host and surface:** the frame, a pane, a card, or an overlay. Use the Band metrics; share the host's
  background, with no independent fill or elevation. Children can have their own component surfaces.
- **States:** a band has no selection or hover state of its own; its controls own those states.
  Toolbar actions use button styling, without an editing border.

#### Chip

- **Purpose and anatomy:** a compact representation of an item, attribute, or choice: a label, optional
  leading icon or count.
- **Host and surface:** within a band, pane, card, field group, or overlay. Use the Chip metrics and
  local tone steps; a chip is a nested surface, not an overlay because it can be clicked.
- **States:** descriptive, actionable, or selectable as appropriate. Selection uses the accent tint
  without a tick or editing border.

#### Tab and tab list

- **Purpose and anatomy:** a tab selects one of a set of related panels. A tab list contains the tab
  labels, optional icons or counts, and one selected tab associated with the visible panel.
- **Host and surface:** a band on the frame, a pane, or within an overlay. Tabs may reuse Chip geometry
  but the tab list shares its host's background. Selected tabs use the 12 % accent tint, 16 % when
  hovered; unselected hover uses a neutral tone. Selection adds neither a border nor elevation.
- **States:** selected, unselected, hovered, or disabled. Selection identifies the associated visible
  panel without a tick icon. Optional close controls are distinct actions.

#### Button and link

- **Purpose and anatomy:** a button invokes an action; a link navigates to a destination. Both need a
  meaningful label, optionally accompanied or replaced by a named icon.
- **Host and surface:** any primary or component surface. Buttons share the Button metrics and one
  neutral treatment, without an action hierarchy. Icon-only controls retain their compact form;
  semantic destructive text and link styling follow §3.
- **States:** rest, hover, pressed, busy, or disabled. Choice and toggle buttons can also be selected;
  an ordinary action button is not selected merely because it is pressed. Buttons have no editing
  border, including an action labeled "Edit."

#### Field

- **Purpose and anatomy:** accepts or displays an editable value: a persistent label, input area and
  cursor, optional hint, validation message, and attached controls. Fields may be single- or multiline.
- **Host and surface:** on a pane, in a card, or in an overlay. Use the Field metrics and nested surface
  treatment. The accent border belongs only to the active editable area, not its attached buttons or
  surrounding container.
- **States:** empty, populated, actively edited, read-only, disabled, or invalid. Inactive and
  read-only fields have no editing border. Explain invalid input with associated text, rather than
  changing color alone.

#### Card

- **Purpose and anatomy:** groups related content. The heading and metadata sit on the outer card;
  everything below them, including actions or choices, sits inside a rounded body surface.
- **Host and surface:** within a pane or overlay, without elevation. The outer card has 4px corners;
  the body has 2px corners and a 2px gap on each side, at the bottom, and below the heading. Its 2px
  padding keeps content aligned with the heading's 4px inset. Status cards use different tones of the
  same status color on both surfaces (§3); the gap replaces the old header hairline.
- **States:** soft or status-bearing. A card is not inherently an action; its controls own hover,
  selection, and disabled states. Use a distinct control for selection or expansion rather than
  treating the card and all its nested actions as one button.

#### Table

- **Purpose and anatomy:** tabular content with aligned columns, bold column headings, and regular
  body text. A surrounding card is not required.
- **Host and surface:** directly on its content host. The header uses the same background as a soft
  card's heading (§3); the body shares its host's background. Use a 1px outer border with 4px corners,
  vertical column dividers, and a divider beneath the header, all in the text color at 12 %.
  Clip the contents to the rounded outline. No horizontal body-row dividers or alternating fills.
- **Metrics:** header and body rows use the shared text-row height. Cells have 4px horizontal padding,
  omitted at the table's outer text edges; no vertical padding. Text aligns left and numeric values
  right. Use the shared overflow treatment for content wider than its host.
- **States:** read-only content; no sorting, selection, or editing controls are implied.

#### Menu and menu item

- **Purpose and anatomy:** presents commands or choices after an explicit trigger: rows, optional
  group labels and dividers, shortcut hints, and submenu indicators.
- **Host and surface:** anchored to a control or contextual target on any host. Use Overlay metrics;
  menu items are one text row high, with 4px side padding and corners. A submenu is another overlay
  above its parent, not another surface role.
- **States:** closed or open; items can be hovered, selected, expanded, or disabled. Choice items use
  the accent selection tint, without added ticks or editing borders. An open trigger is not
  automatically a selected choice.

#### Hover

- **Purpose and anatomy:** a short floating label for content, with an optional action. Without an
  action it displays information; with an action it is clickable. Jump to latest is one use of the
  actionable form, not a separate component.
- **Host and surface:** Overlay + Text. One text row high, with no
  vertical padding, 4px horizontal padding, and 6px corners. Use the overlay color, shadow, and
  material treatment (§5); text keeps its normal opacity. Both forms share these metrics, without a
  nested button or a new surface role.
- **States:** hidden or visible; informational or actionable. No editing border or selection tint.

#### Scrollbar

- **Purpose and anatomy:** indicates and changes the visible portion of a scrollable region: a track,
  proportional thumb, and optional content markers.
- **Host and surface:** local to the region it scrolls, using Indicator metrics. The thumb remains
  quiet at rest; its track and markers reveal on interaction without creating a floating surface.
- **States:** rest, hover, or dragging. The thumb and markers describe the scrollable content; the
  implementation determines input behavior.

#### Minimap

- **Purpose and anatomy:** a miniature content view with a visible-range band and optional change
  and diagnostic markers. It is part of its content view, not another surface role.
- **Surface:** the content's overlay color, 100 % opaque even on a translucent host. Use 4px corners,
  with 4px top and bottom padding. No blur or shadow.
- **Lines:** each occupied text column is a 1 × 1px block in its content color. Preserve indentation
  and spaces; blank lines remain blank. The vertical pitch is 2px: one pixel of ink and one of space.
- **Size:** a 4px change lane, the text columns at 1px each, then a 4px diagnostic lane. Cap the text
  area at 120 columns (128px total); clip longer lines rather than wrapping them. Height follows the
  available content-view height.
- **Current view:** a band behind the text blocks and confined to their area, positioned and sized
  from the visible lines. Use text color at 8 % opacity at rest and 16 % under the pointer. The
  surface stays opaque; the text and marker colors and opacity do not change with the band.
- **Left markers:** added or changed ranges use a 2px-wide bar, 2px per affected line and at least
  4px high. Deletions use a 4 × 2px tick. Keep them within the left lane, in the content's change colors.
- **Right markers:** diagnostics use 4px dots in the right lane, centered on their line. Color follows
  severity; the more severe marker takes precedence where dots overlap. Keep markers inside the map.
- **Alignment:** text, markers, and viewport share one line origin: top padding plus
  (line − first mapped line) × line pitch. Markers remain visible while the map is shown, outside the
  text area. For long documents, retain the line pitch and show a moving section around the current
  view; all parts move together rather than using different vertical scales.

## 10. Approved implementation profiles

A named profile is a normative, scoped replacement for the shared defaults it lists. All
unlisted surface roles, component anatomy, color and state rules, accessibility requirements, and
performance commitments remain unchanged. Do not transfer a profile's rules outside its stated scope or interpolate
additional values from it. qvim applies the shared chrome profile to both platforms, with the Android geometry profile
on Android and the desktop chat text-alignment profile only inside desktop chat.

### 10.1. Android touch profile

The Android profile preserves the component roles in §9 while adapting visual geometry to touch
screens. Visual faces can be smaller than their hit areas; every interactive target remains at least
48 × 48 logical pixels.

#### Single-pane surface

Single-pane Android views use one continuous content surface. The pane fills the application view.
Frame chrome, including title and bottom-navigation bands, shares the pane's background; omit a
separate frame fill and frame-to-pane gap. Content padding and minimum touch areas still apply.
Cards retain their nested surfaces, and genuine overlays retain their elevation.

#### Density, radius, and padding

- Card and search-field corners are 14px.
- The persistent composer has 18px corners.
- Outer cards and selected code blocks use 8px padding.
- Code blocks and images inside cards use 4px corners.
- Metadata badges use 1px vertical and 4px horizontal padding, with 4px corners.
- Soft/status cards, action tiles, list rows, and search fields reserve a 1px non-painted layout edge. Their padding is
  measured inside that edge; it is not a visible outline. User-message bubbles have no reserved edge.
- Component families use the exact values in this profile rather than one global compact or roomier
  density. Do not introduce additional intermediate density values.

#### Card body

- An Android card with a distinct heading uses one inset body for everything below the heading.
- The body begins 2px below the heading and remains 2px from the outer card at the sides and bottom.
- The outer card keeps its 14px corners. The body uses concentric 12px corners.
- Body content keeps the card's horizontal content inset; the body surface does not add another horizontal padding.
  Code blocks retain their own padding.
- A soft card body uses the content background. A status card body follows the shared same-hue rule
  in §3 unless the qvim chrome profile applies.
- Action tiles and list rows without a distinct heading remain single nested surfaces.

#### Screen gutters and content inset

- Screen content uses 16px side gutters.
- Content insets follow the screen-level Android layout rather than the default frame gap and pane
  text inset.

#### Bands and content spacing

- Selected compact visual headers are 25px high.
- Top-level conversation parts sit approximately 20px apart.
- App lists and card groups use an 8px gap where the screen's compact grouping requires it.
- Home tiles use 6px between their stacked icon, content, and action groups; related text within a group uses 4px.
- Conversation list items and adjacent metadata badges use 4px spacing.

#### Typography

- Android keeps one UI typeface but may use role-based font sizes.
- Screen titles may be larger than body text; metadata, compact tools, chips, and context labels may
  be smaller than body text.
- A role uses one consistent size across screens, and text remains legible at the supported display
  and accessibility scales.
- This clause overrides the shared single-size rule only while §4 requires one UI size. If the shared
  language adopts role-based UI sizes, remove this duplicate Android override.

#### Control geometry

- Prompt selectors and compact action faces are 25px high inside 48px touch areas.
- Compact action faces use 6px horizontal padding; prompt-selector faces use 4px padding.
- Permission action faces are 32px high with 8px corners and 8px horizontal padding, inside 48px touch areas.
- Jump and working Stop use circular visual faces.
- Every interactive target remains at least 48 × 48 logical pixels.

#### Table geometry

- Results tables use 12px outer corners.
- Table cells use 8px horizontal and 4px vertical padding.

#### Context indicator

- The indicator is 3px high.
- Its used portion is an accent fill; unused capacity stays transparent, without a separate thumb.

#### Composer material

- The persistent composer keeps 72 % of its surface color over a 32px blur.
- The material adds saturation and a stronger multi-layer shadow than the default overlay.

### 10.2. qvim chrome profile

**Scope:** the approved qvim editor, desktop chat, and Android reference screens. This profile overrides the
percentage-tint recipes, status-card hue requirements, and region-outline color source in §§2, 3, and 9; it does not
change their component roles or geometry.

- **Chrome tones:** use the named opaque colors and role aliases in §11. Frame, resting, raised, selection, and status
  tones are fixed palette values, not new mixtures calculated against each host. Their lightness-only derivations are
  normative; the shared percentage recipes are not used to regenerate them.
- **Content-owned colors:** Neovim owns buffer content, pane background, primary foreground, cursor,
  text-range selection, and overlay/menu colors where defined. Resolve these from content, with §11 as fallback.
  Changing the content theme does not recalculate the fixed chrome tones.
- **Information and status:** information, error, warning, success, and change roles use the shared palette defaults.
  A theme's non-diff buffer syntax remains content-owned; UI roles and addition/deletion markings use §11 across qvim.
- **Materials and marks:** material opacity, structural hairlines, and minimap viewport bands keep their own recipes.
  They are not additional palette entries or permission to make control/state fills translucent.
- **Region outlines:** use the named soft-outer tone, not necessarily the raised-control tone.
- **Selected text:** selected menu metadata uses primary text. Inactive and disabled content uses the single muted tone.
- **Status cards:** outer and body surfaces may be neutral. Use semantic text or glyph color with a meaningful label or
  shape; apply the foreground pairings in §11.3 instead of the shared same-hue card requirement.

### 10.3. qvim desktop chat text alignment

**Scope:** text alignment inside desktop chat only. This replaces the fixed lead-gap rule at the boundaries below,
not the frame gap, surface padding, corners, band heights, or content-group spacing. Android does not use this profile.

The chat shares the editor's monospaced typeface, cell width, and row height. A cell is a text measure, not a density
token. Glyph-based rows and chips can reserve whole text cells between their marks, labels, and metadata:

| Text boundary | Alignment |
|---------------|-----------|
| Background output indentation | 4 cells |
| Bulleted-list hanging indentation | 2 cells |
| Divider label to its hairlines | 1 cell |
| Glyph to label in a chat row, card heading, or chip | 1 cell |
| Menu or quota metadata separation | 2 cells |
| Nested tool output and expanded thinking indentation | 2 cells |
| Numbered-list hanging indentation | 3 cells |
| Plan count groups | 2 cells; compact inline progress uses 1 |
| Right-aligned row metadata and status controls | At least 1 cell |
| Title-chip trailing chevron or additional-session mark | 1 cell |

Standalone identity artwork in the title chip keeps the shared lead gap. Text columns may be sized in cells, and
content such as code preserves its own whitespace. Expanded background output reserves a 28px trailing action lane
beside its text. At the reference font, one cell is 11px and one row is 25px; the cell-based values follow the font,
while the unchanged geometry tokens still follow display scaling.

## 11. qvim reference palette

The canonical palette contains **29 unique colors: 17 primary and 12 derived** across dark and light themes.
Each value has one row; shared roles alias that row rather than adding colors. Content-theme overrides allowed by
§10.2 are not additional canonical palette entries.

Primary colors are independently chosen. A derived color changes only the HSL lightness of its named primary:
preserve hue and saturation, set the listed lightness, then round RGB channels to the nearest 8-bit value, with exact
halves rounded up. All palette values are opaque; do not blend them with a background or reduce their opacity.

The palette excludes non-diff colors inside Neovim-rendered buffer content, file-type icons, screenshots, wallpaper
gradients, review annotations, host window edges, caption controls, materials, and structural borders.
Brand and provider artwork colors are outside this chrome palette.

### 11.1. Primary colors

| Theme | Color | Primary roles and consolidated usages | Systems |
|-------|-------|--------------------------------------|---------|
| Dark | `#8B9DFF` | Accent: active focus/editing, working state, scroll/overflow thumbs, context-used fill | Editor, chat, app |
| Dark | `#F6B3B8` | Error/failure: diagnostics, failed states, deleted-line fills and deletion markers | Editor, chat, app |
| Dark | `#080A0C` | Frame/base: solid desktop frame, caption band and gaps around panes | Editor, chat |
| Dark | `#8CF8F7` | Information/change: diagnostics, changed-state markers, paths and mentions | Editor, chat |
| Dark | `#1C1F25` | Pane/background: fallback content surfaces, app background, inset bodies and read-only fields | Editor, chat, app |
| Dark | `#2F323A` | Raised/surface: controls, app surfaces, interaction/neutral/quota tracks, region outlines, soft-card exteriors and table headers | Editor, chat, app |
| Dark | `#B5E3C8` | Success: completed/connected states, added-line fills and addition markers | Editor, chat, app |
| Dark | `#EFF3F8` | Text: fallback primary copy, labels and neutral control text | Editor, chat, app |
| Dark | `#FCE094` | Warning: warning and needs-attention text, glyphs and badge text | Editor, chat, app |
| Light | `#3545B8` | Accent: active focus/editing, working state, scroll/overflow thumbs, context-used fill | Editor, chat, app |
| Light | `#590008` | Error/failure: diagnostics, failed states, deleted-line fills and deletion markers | Editor, chat, app |
| Light | `#E4E7EC` | Raised/surface: controls, app surfaces, interaction tracks, status exteriors, open status fills and warning badges | Editor, chat, app |
| Light | `#006E6E` | Information/change: diagnostics, changed-state markers, paths and mentions | Editor, chat |
| Light | `#F7F8FA` | Pane/background: fallback content surfaces, app background, inset bodies and read-only fields | Editor, chat, app |
| Light | `#005523` | Success: completed/connected states, added-line fills and addition markers | Editor, chat, app |
| Light | `#0C0F14` | Text: fallback primary copy, labels and neutral control text | Editor, chat, app |
| Light | `#6B5300` | Warning: warning and needs-attention text, glyphs and badge text | Editor, chat, app |

### 11.2. Derived colors

`L` is absolute HSL lightness, not opacity or a percentage of the source color.

| Theme | Derived color | Role | Primary color | Lightness-only formula |
|-------|---------------|------|---------------|------------------------|
| Dark | `#9CA2AF` | Muted text | Raised `#2F323A` | Brighten to `L = 65%` |
| Dark | `#23272E` | Resting, status-body and message tone | Pane `#1C1F25` | Brighten to `L = 16%` |
| Dark | `#0020CC` | Selection and working fill | Accent `#8B9DFF` | Dim to `L = 40%` |
| Dark | `#32353E` | Status outer | Raised `#2F323A` | Brighten to `L = 22%` |
| Dark | `#1C4A2F` | Success status fill | Success `#B5E3C8` | Dim to `L = 20%` |
| Dark | `#634903` | Warning status fill | Warning `#FCE094` | Dim to `L = 20%` |
| Light | `#CFD4DD` | Frame/base, region outlines, neutral rest and soft exterior | Raised `#E4E7EC` | Dim to `L = 84%` |
| Light | `#455673` | Muted text | Text `#0C0F14` | Brighten to `L = 36%` |
| Light | `#ECEFF3` | Resting, status-body and message tone | Pane `#F7F8FA` | Dim to `L = 94%` |
| Light | `#D7DBF4` | Selection and working fill | Accent `#3545B8` | Brighten to `L = 90%` |
| Light | `#B3FFD2` | Success status fill | Success `#005523` | Brighten to `L = 85%` |
| Light | `#FFEEB3` | Warning status fill | Warning `#6B5300` | Brighten to `L = 85%` |

### 11.3. Foreground pairings

- Diff lines use Success for added fills and Error for deleted fills. Code, line numbers and signs use the theme's
  palette Pane color as contrasting text; diff counts and markers use Success/Error directly.
- Neutral and message surfaces use primary text, or muted text for secondary content. Selection surfaces use primary
  text for secondary content too.
- Shared status-card exteriors stay neutral; primary Error or Warning identifies the status.
- Success and Warning chip fills use their matching primary status color for text and glyphs, not muted text.

See the [qvim application guide](implementation.md#implementation-mapping) for implementation bindings.
