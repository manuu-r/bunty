// Bunty's reaction pictures: tiny looping GIFs small enough to travel inside the
// speech stream. They are drawn here rather than stored, so the art stays
// reviewable, and the device's zoom scales 48x64 up by exactly five to fill its
// 240x320 screen. Flat colour and few frames keep each one under a kilobyte or so.
const WIDTH = 48;
const HEIGHT = 64;

const INK = { bg: 0, ivory: 1, mint: 2, lavender: 3, gold: 4, sky: 5, coral: 6, shade: 7 };
const PALETTE = [
  [6, 8, 15], // the face's background, so a picture sits on the same dark
  [233, 241, 223],
  [193, 225, 156],
  [168, 177, 245],
  [250, 204, 92],
  [122, 190, 245],
  [240, 110, 96],
  [84, 92, 112],
];

class Canvas {
  constructor() {
    this.pixels = new Uint8Array(WIDTH * HEIGHT);
  }

  dot(x, y, ink) {
    const px = Math.round(x);
    const py = Math.round(y);
    if (px >= 0 && py >= 0 && px < WIDTH && py < HEIGHT) this.pixels[py * WIDTH + px] = ink;
  }

  /** Fill every pixel the predicate accepts. */
  fill(inside, ink) {
    for (let y = 0; y < HEIGHT; y += 1) {
      for (let x = 0; x < WIDTH; x += 1) {
        if (inside(x, y)) this.pixels[y * WIDTH + x] = ink;
      }
    }
  }

  disc(cx, cy, radius, ink) {
    this.fill((x, y) => (x - cx) ** 2 + (y - cy) ** 2 < radius ** 2, ink);
  }

  ring(cx, cy, radius, width, ink) {
    this.fill((x, y) => Math.abs(Math.hypot(x - cx, y - cy) - radius) <= width / 2, ink);
  }

  rect(x0, y0, x1, y1, ink) {
    this.fill((x, y) => x >= x0 && x <= x1 && y >= y0 && y <= y1, ink);
  }

  line(x0, y0, x1, y1, radius, ink) {
    const steps = Math.ceil(Math.hypot(x1 - x0, y1 - y0));
    for (let step = 0; step <= steps; step += 1) {
      const t = steps ? step / steps : 0;
      this.disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, radius, ink);
    }
  }

  // Four-pointed twinkle.
  star(cx, cy, size, ink) {
    this.fill((x, y) => Math.abs(x - cx) ** 0.6 + Math.abs(y - cy) ** 0.6 <= size ** 0.6, ink);
  }

  cloud(cx, cy, ink) {
    this.disc(cx - 9, cy + 2, 7, ink);
    this.disc(cx, cy - 3, 10, ink);
    this.disc(cx + 10, cy + 1, 8, ink);
    this.rect(cx - 15, cy + 2, cx + 17, cy + 8, ink);
  }
}

const TAU = Math.PI * 2;

// name: [frames, centiseconds per frame, draw(canvas, frame, phase 0..1)]
const PICTURES = {
  sun: [4, 22, (c, _f, p) => {
    for (let ray = 0; ray < 8; ray += 1) {
      const angle = (ray / 8 + p / 8) * TAU;
      c.line(24 + Math.cos(angle) * 15, 30 + Math.sin(angle) * 15,
        24 + Math.cos(angle) * 20, 30 + Math.sin(angle) * 20, 1, INK.gold);
    }
    c.disc(24, 30, 11, INK.gold);
  }],
  moon: [4, 30, (c, f) => {
    c.disc(23, 30, 15, INK.ivory);
    c.disc(30, 25, 13, INK.bg);
    c.star(38, 44, f % 2 ? 3 : 1, INK.gold);
    c.star(9, 14, f % 2 ? 1 : 3, INK.gold);
  }],
  cloud: [4, 30, (c, f) => {
    c.cloud(22 + [0, 1, 2, 1][f], 28, INK.ivory);
  }],
  rain: [4, 13, (c, f) => {
    c.cloud(23, 20, INK.lavender);
    for (const [x, start] of [[12, 0], [20, 6], [28, 2], [36, 8]]) {
      const y = 33 + ((start + f * 5) % 20);
      c.line(x, y, x - 2, y + 6, 1.3, INK.sky);
    }
  }],
  snow: [4, 22, (c, f) => {
    c.cloud(23, 20, INK.ivory);
    for (const [x, start] of [[11, 0], [19, 7], [27, 3], [36, 10]]) {
      c.disc(x + (f % 2), 34 + ((start + f * 5) % 20), 2, INK.ivory);
    }
  }],
  storm: [4, 16, (c, f) => {
    c.cloud(23, 19, INK.shade);
    if (f === 1 || f === 2) {
      c.line(25, 28, 19, 40, 1.1, INK.gold);
      c.line(19, 40, 27, 40, 1.1, INK.gold);
      c.line(27, 40, 20, 54, 1.1, INK.gold);
    }
  }],
  heart: [4, 18, (c, f) => {
    const scale = 13 + [0, 2, 3, 2][f];
    c.fill((x, y) => {
      const u = (x - 24) / scale;
      const v = (31 - y) / scale;
      return (u * u + v * v - 1) ** 3 - u * u * v * v * v <= 0;
    }, INK.coral);
  }],
  sparkle: [4, 18, (c, f) => {
    const stars = [[24, 30, 12, 0], [10, 14, 6, 1], [38, 12, 7, 2], [9, 48, 6, 3], [38, 49, 8, 1]];
    for (const [x, y, size, offset] of stars) {
      const beat = [1, 0.75, 0.5, 0.75][(f + offset) % 4];
      c.star(x, y, size * beat, offset === 1 ? INK.mint : INK.gold);
    }
  }],
  party: [4, 14, (c, f) => {
    const inks = [INK.coral, INK.gold, INK.mint, INK.sky, INK.lavender];
    for (let piece = 0; piece < 16; piece += 1) {
      const x = 3 + ((piece * 29) % 42);
      const y = (((piece * 37) % 64) + f * 16) % 64;
      c.rect(x, y, x + 2 + (piece + f) % 2, y + 2, inks[piece % inks.length]);
    }
  }],
  tear: [4, 22, (c, f) => {
    const y = 14 + f * 5;
    c.disc(24, y + 16, 12, INK.sky);
    c.fill((px, py) => py >= y - 10 && py <= y + 10 && Math.abs(px - 24) <= (py - y + 10) * 0.52, INK.sky);
    if (f === 3) c.line(8, 56, 40, 56, 1.1, INK.sky);
  }],
  fire: [4, 12, (c, f) => {
    const sway = [0, 2, 0, -2][f];
    const flame = (height, half, ink) => {
      c.fill((x, y) => {
        const t = (52 - y) / height;
        if (t < 0 || t > 1) return false;
        return Math.abs(x - 24 - sway * t) <= half * Math.sin(Math.PI * t ** 0.6);
      }, ink);
    };
    flame(40, 15, INK.coral);
    flame(27, 10, INK.gold);
    flame(13, 5, INK.ivory);
  }],
  coffee: [4, 24, (c, f) => {
    c.rect(11, 32, 31, 52, INK.ivory);
    c.ring(34, 41, 6, 2.4, INK.ivory);
    c.rect(11, 31, 31, 34, INK.shade);
    for (const x of [16, 25]) {
      for (let y = 12; y < 28; y += 1) {
        c.dot(x + Math.sin((y / 16 + f / 4) * TAU) * 2, y, INK.lavender);
      }
    }
  }],
  food: [4, 24, (c, f) => {
    c.fill((x, y) => y >= 34 && (x - 24) ** 2 / 400 + (y - 34) ** 2 / 324 <= 1, INK.coral);
    c.rect(4, 33, 44, 35, INK.gold);
    for (const x of [15, 24, 33]) {
      for (let y = 12; y < 30; y += 1) {
        c.dot(x + Math.sin((y / 18 + f / 4 + x / 30) * TAU) * 2, y, INK.ivory);
      }
    }
  }],
  music: [4, 20, (c, f) => {
    const note = (x, y, ink) => {
      c.disc(x, y, 4.5, ink);
      c.line(x + 4, y, x + 4, y - 20, 0.7, ink);
      c.line(x + 4, y - 20, x + 10, y - 16, 0.9, ink);
    };
    note(14, 40 + [0, -3, 0, 3][f], INK.mint);
    note(29, 48 + [0, 3, 0, -3][f], INK.lavender);
  }],
  idea: [4, 24, (c, f) => {
    if (f % 2) {
      for (let ray = 0; ray < 7; ray += 1) {
        const angle = Math.PI + (ray / 6) * Math.PI;
        c.line(24 + Math.cos(angle) * 18, 25 + Math.sin(angle) * 18,
          24 + Math.cos(angle) * 22, 25 + Math.sin(angle) * 22, 0.8, INK.gold);
      }
    }
    c.disc(24, 25, 13, f % 2 ? INK.gold : INK.ivory);
    c.rect(19, 36, 29, 41, f % 2 ? INK.gold : INK.ivory);
    c.rect(19, 43, 29, 45, INK.shade);
    c.rect(21, 47, 27, 49, INK.shade);
  }],
  check: [4, 16, (c, f) => {
    c.ring(24, 32, 20, 3, INK.mint);
    if (f >= 0) c.line(13, 32, 21, 41, 1.8, INK.mint);
    if (f >= 1) c.line(21, 41, 29, 31, 1.8, INK.mint);
    if (f >= 2) c.line(29, 31, 36, 23, 1.8, INK.mint);
  }],
  cross: [4, 12, (c, f) => {
    const x = [24, 21, 27, 24][f];
    c.ring(x, 32, 20, 3, INK.coral);
    c.line(x - 9, 23, x + 9, 41, 1.8, INK.coral);
    c.line(x + 9, 23, x - 9, 41, 1.8, INK.coral);
  }],
  clock: [8, 22, (c, f) => {
    c.ring(24, 32, 20, 3, INK.ivory);
    c.line(24, 32, 24, 21, 1, INK.ivory);
    const angle = (f / 8) * TAU;
    c.line(24, 32, 24 + Math.sin(angle) * 15, 32 - Math.cos(angle) * 15, 0.7, INK.coral);
  }],
  calendar: [4, 28, (c, f) => {
    c.rect(6, 14, 41, 52, INK.ivory);
    c.rect(6, 14, 41, 22, INK.coral);
    c.rect(13, 10, 15, 17, INK.shade);
    c.rect(32, 10, 34, 17, INK.shade);
    for (let row = 0; row < 3; row += 1) {
      for (let col = 0; col < 4; col += 1) {
        const lit = row === 1 && col === 2 && f % 2 === 0;
        const x = 10 + col * 7;
        const y = 27 + row * 8;
        c.rect(x, y, x + 3, y + 3, lit ? INK.coral : INK.shade);
      }
    }
  }],
  mail: [4, 20, (c, f) => {
    const y = 20 + [0, -2, -3, -2][f];
    c.rect(5, y, 42, y + 25, INK.ivory);
    c.line(5, y, 24, y + 14, 0.8, INK.shade);
    c.line(42, y, 24, y + 14, 0.8, INK.shade);
  }],
  search: [6, 22, (c, f) => {
    const x = 20 + Math.sin((f / 6) * TAU) * 5;
    const y = 26 + Math.cos((f / 6) * TAU) * 3;
    c.ring(x, y, 13, 3, INK.lavender);
    c.line(x + 10, y + 10, x + 19, y + 19, 1.8, INK.lavender);
  }],
  rocket: [4, 12, (c, f) => {
    for (const [x, start] of [[8, 0], [40, 9], [15, 16], [34, 3]]) {
      c.dot(x, (start + f * 8) % 64, INK.ivory);
    }
    c.fill((x, y) => y >= 8 && y <= 42 && Math.abs(x - 24) <= 8 * Math.sin(Math.PI * Math.min(1, (y - 6) / 44)), INK.ivory);
    c.disc(24, 24, 3, INK.sky);
    c.line(16, 38, 11, 47, 1.2, INK.coral);
    c.line(32, 38, 37, 47, 1.2, INK.coral);
    c.line(24, 44, 24, 49 + (f % 2) * 5, 2, INK.gold);
  }],
};

export const PICTURE_NAMES = Object.keys(PICTURES);

// With no picture tag on a reply, its feeling still gets an effect.
const EMOTION_PICTURES = {
  excited: "party",
  happy: "sparkle",
  soft: "heart",
  sad: "tear",
  angry: "fire",
  curious: "search",
};

/** @param {string} emotion */
export function pictureForEmotion(emotion) {
  return EMOTION_PICTURES[emotion] ?? null;
}

/** LZW as GIF wants it: variable-width codes, least significant bit first. */
function lzw(indices, out) {
  const minimum = 3; // eight colours
  const clear = 1 << minimum;
  const finish = clear + 1;
  let table = new Map();
  let next = finish + 1;
  let width = minimum + 1;
  let accumulator = 0;
  let bits = 0;
  const bytes = [];
  const emit = (code) => {
    accumulator |= code << bits;
    bits += width;
    while (bits >= 8) {
      bytes.push(accumulator & 0xff);
      accumulator >>>= 8;
      bits -= 8;
    }
  };

  emit(clear);
  let prefix = indices[0];
  for (let index = 1; index < indices.length; index += 1) {
    const value = indices[index];
    const key = (prefix << 3) | value;
    const known = table.get(key);
    if (known !== undefined) {
      prefix = known;
      continue;
    }
    emit(prefix);
    if (next === 4096) {
      emit(clear);
      table = new Map();
      next = finish + 1;
      width = minimum + 1;
    } else {
      // A decoder is one entry behind, so widen once the next code won't fit.
      if (next === 1 << width && width < 12) width += 1;
      table.set(key, next);
      next += 1;
    }
    prefix = value;
  }
  emit(prefix);
  emit(finish);
  if (bits > 0) bytes.push(accumulator & 0xff);

  out.push(minimum);
  for (let offset = 0; offset < bytes.length; offset += 255) {
    const block = bytes.slice(offset, offset + 255);
    out.push(block.length, ...block);
  }
  out.push(0);
}

const word = (value) => [value & 0xff, value >> 8];

/** Smallest rectangle holding every change from the previous frame. */
function changedArea(previous, pixels) {
  if (!previous) return { x: 0, y: 0, width: WIDTH, height: HEIGHT };
  let left = WIDTH;
  let right = -1;
  let top = HEIGHT;
  let bottom = -1;
  for (let y = 0; y < HEIGHT; y += 1) {
    for (let x = 0; x < WIDTH; x += 1) {
      if (previous[y * WIDTH + x] === pixels[y * WIDTH + x]) continue;
      left = Math.min(left, x);
      right = Math.max(right, x);
      top = Math.min(top, y);
      bottom = Math.max(bottom, y);
    }
  }
  if (right < 0) return { x: 0, y: 0, width: 1, height: 1 };
  return { x: left, y: top, width: right - left + 1, height: bottom - top + 1 };
}

function encode(frames, delay, draw) {
  const out = [...new TextEncoder().encode("GIF89a"), ...word(WIDTH), ...word(HEIGHT), 0xf2, 0, 0];
  out.push(...PALETTE.flat());
  out.push(0x21, 0xff, 11, ...new TextEncoder().encode("NETSCAPE2.0"), 3, 1, 0, 0, 0);

  let previous = null;
  for (let frame = 0; frame < frames; frame += 1) {
    const canvas = new Canvas();
    draw(canvas, frame, frame / frames);
    const area = changedArea(previous, canvas.pixels);
    out.push(0x21, 0xf9, 4, 0x04, ...word(delay), 0, 0); // disposal 1: keep this frame
    out.push(0x2c, ...word(area.x), ...word(area.y), ...word(area.width), ...word(area.height), 0);
    const indices = [];
    for (let y = area.y; y < area.y + area.height; y += 1) {
      for (let x = area.x; x < area.x + area.width; x += 1) {
        indices.push(canvas.pixels[y * WIDTH + x]);
      }
    }
    lzw(indices, out);
    previous = canvas.pixels;
  }
  out.push(0x3b);
  return Uint8Array.from(out);
}

const encoded = new Map();

/** @param {string} name @returns {Uint8Array | null} */
export function pictureGif(name) {
  if (!Object.hasOwn(PICTURES, name)) return null;
  const picture = PICTURES[name];
  if (!encoded.has(name)) encoded.set(name, encode(...picture));
  return encoded.get(name);
}
