# Bunty screen shell and Muse tools

> `cfg::ui::kTextContent` is false: the screen shows the face and pictures only.
> Everything below is withheld from Muse, and the Recent handle is hidden. Muse
> says this information aloud instead. Setting the flag to true restores it all.

The animated face is the default home. Say "Hey Buddy" to start voice; tapping the face does nothing. Tap the
small bottom handle to open Recent; it contains only previously requested
Agenda, Tasks, Notes and Info views. No clock dashboard, empty app tiles,
summary cards or automatic data fetches appear at startup. A long press opens
Settings when idle and ends an active conversation. The first touch on a dimmed
screen only wakes it.

Content uses one title, optional source, plain rows and a quiet microphone
shortcut. Tap a row for detail, a task checkbox to mark a pending provider
change, and the arrows to page. Back returns to the list, then the face. Talk
returns to the active conversation without discarding requested content.
The listening border stays visible over content. Settings and pairing use the
same restrained type, colours and flat controls.

[Six-screen preview](docs/bunty-shell-preview.png) shows empty/loaded Recent,
Agenda, Tasks, note detail and Info using sample data and the real renderer.
No sample records are installed in firmware. The face preview is in
[VOICE.md](VOICE.md). Snapshots arrive only through explicit Muse tools;
each voice request instructs Muse to show only content the user asks for.

## Muse tools

| Tool | Behavior |
| --- | --- |
| `ui.set_agenda` | Replace an ordered snapshot of up to 12 calendar events; the first is Next Up. |
| `ui.set_tasks` | Replace up to 12 tasks, preserving pending local checkmarks until explicitly acknowledged. |
| `ui.set_notes` | Replace up to 12 notes, each with paged detail. |
| `ui.show_card` | Show a title, primary value, subtitle and paged body. Suitable for weather, reminders, travel and briefings. |
| `ui.open` | `home`/`face` restores the companion; `apps` opens Recent. Data views open only if already loaded. |
| `ui.set_context` | Set `utc_offset_minutes` between -720 and +840; initial +330. |
| `ui.get_state` | Read active view, loaded flags, counts, tasks and task revision. |
| `bunty.gesture` | Queue `neutral`, `nod`, `shake`, `curious` or `happy` for silence or images. Spoken replies use expression tags. |

Muse must fetch actual calendar/task/note provider data and send snapshots. This
firmware does not create a provider connection or periodic refresh. Snapshots
and pending task actions live in RAM and disappear on restart. Unsupported
Unicode becomes `?` in bitmap text; use the existing `display.draw_url` tool for
multilingual layouts and rendered graphics.

All collection items need unique `id` (1–48 bytes) and `title` (1–80 UTF-8 bytes).
Optional fields: `time` (24 bytes), `meta` (64), `body` (384), `done` (boolean).
Collection `title` and `source` are at most 40 bytes. Muse should format times in
the user's timezone and order events for display. `open:false` updates the data
without switching views; the default opens it immediately.

Example `ui.set_agenda` parameters:

```json
{
  "title": "Today",
  "source": "Calendar",
  "items": [{"id":"event-42","title":"Design review","time":"14:30","meta":"Studio · 30 min","body":"Bring the latest prototype."}]
}
```

A local task check is displayed as pending. Muse reads `ui.get_state`, applies
its pending task IDs/done values through the real provider, and only after
success resends `ui.set_tasks` with `ack_revision` equal to that state response's
`task_revision`. A newer local change makes the acknowledgment stale and the
replacement is rejected. Failed provider writes must remain unacknowledged.
There is no unsolicited task push to Muse; it must read the state.

`ui.show_card` requires `title` (40 bytes); optional `value` (24), `subtitle`
(80), `body` (768), `source` (40), and `open` work as above. For example:

```json
{"title":"Weather","value":"26 C","subtitle":"Light rain","body":"Carry an umbrella for your afternoon walk.","source":"Weather provider"}
```

## Speech and head movement

Every outgoing audio request includes a text instruction asking Muse to choose
`bunty.gesture` alongside its concise spoken reply, call it before replying, and
keep movement markup out of speech. Tool execution depends on Muse honoring the
instruction; sending the instruction alone cannot guarantee a gesture.

The gesture tool takes `{"gesture":"nod"}` (or another name above). It uses a
single atomic mailbox, plays when speech starts, expires after 20 seconds, and
is cleared at cancellation or the next listening turn. Each gesture lasts at
most two seconds, uses offsets up to 12 degrees and speed at most 60 degrees/s.
All conversation poses are additionally clamped to ±18 degrees yaw and ±15
pitch under the existing Dance power reservation and calibrated servo limits.
`bunty.look` remains the idle manual positioning tool; those requests wait until
the conversation controller finishes homing. It does not drive competing axes
while Dance owns the head.

## Runtime and verification

Two fixed 23,072-byte models are allocated in PSRAM on the first data/context
update, with checked admission and recoverable failure. Startup, navigation
and reading empty state allocate no content models. Updates validate
in staging and commit atomically under the UI mutex. Invalid payloads preserve
the previous data. Allocation failure retains the face UI. The shell reuses the
existing framebuffer, redraws only on changes, and allocates no bulk
storage while rendering. SDK callbacks update state; the main loop owns drawing,
touch navigation and motion. Images/text retain their existing display ownership.

Host checks exercise the actual parser, command bindings and renderer, including
invalid payloads, OOM, task acknowledgments, pagination, pixel bounds and gesture
clock wrapping/expiry/cancellation. Python checks parse the actual adapted SDK
voice body to verify its gesture instruction and audio attachment. Hardware
verification must still cover touch, servo directions, speech timing, image
ownership and repeated Muse replies. 2026-10-06: the shell/gesture firmware was flashed with hash verification.
Boot verification found the expanded registration was 8,892 bytes, exceeding
the upstream 8 KB cap. Bunty now prints registration into one bounded 32 KB
PSRAM buffer; the rebuilt firmware successfully received `link.register` acknowledgment.
Live touch and spoken gesture timing still need user verification.

The animated Face view and voice attention poses are refreshed in
[FACES.md](FACES.md), with a [rendered preview](docs/bunty-faces-preview.png).

The minimal shell passed host/SDK checks, including no startup content
allocations, recovery after allocation failure, unloaded-view rejection, loaded
Recent navigation, task changes, paging and render bounds. Physical touch and
Muse-driven screen transitions still need on-device verification.

The minimal UI firmware was built and flashed with checksum verification on
2026-10-06. Pairing and stored credentials were preserved.
Startup verification confirmed Muse registration and approximately 6.26 MiB
free PSRAM without loading content models. No panic was observed in the capture.
