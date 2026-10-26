"""Export and verify the canonical dot SVG artwork (requires Microsoft Edge)."""

import argparse
import base64
import io
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright


ICO_SIZES = (16, 32, 48, 64, 128, 256)
MOTION_CHECK = """() => {
  const svg = document.querySelector("svg");
  const animations = svg.getAnimations({subtree: true});
  const eyes = [...svg.querySelectorAll(".a-eye")];
  const centre = node => new DOMPoint(node.cx.baseVal.value, node.cy.baseVal.value)
    .matrixTransform(node.getScreenCTM());
  if (animations.length !== 2 || animations.some(animation => animation.animationName !== "focused-scan")) {
    return false;
  }
  const frames = [0, .5, 1].map(phase => {
    for (const animation of animations) {
      animation.pause();
      animation.currentTime = animation.effect.getTiming().duration * phase;
    }
    return eyes.map(eye => {
      const iris = eye.querySelector(":scope > circle");
      const pupil = eye.querySelector(".a-pu > circle");
      const glint = eye.querySelector(".a-pu > circle[fill='#fff']");
      const lid = eye.querySelector(":scope > path");
      const point = centre(pupil);
      const highlight = glint && centre(glint);
      return {
        attached: !glint || glint.parentElement === pupil.parentElement,
        clipped: Boolean(pupil.closest("[clip-path]")),
        down: point.y > centre(iris).y,
        glint: Boolean(glint),
        lid: lid.getScreenCTM(),
        offset: highlight && [highlight.x - point.x, highlight.y - point.y],
        x: point.x
      };
    });
  });
  return eyes.length === 2 && eyes.every((_, index) => {
    const first = frames[0][index];
    return frames[1][index].x > first.x && frames.every(frame => {
      const sample = frame[index];
      return sample.attached && sample.clipped && sample.down &&
        sample.glint === (svg.dataset.master === "large") &&
        ["a", "b", "c", "d", "e", "f"].every(key => sample.lid[key] === first.lid[key]) &&
        (!sample.offset || sample.offset.every((value, axis) => Math.abs(value - first.offset[axis]) < .0001));
    });
  });
}"""
NS = "http://www.w3.org/2000/svg"
SIZES = (16, 20, 24, 32, 48, 64, 96, 128, 256)
STATES = (("idle", "At rest"), ("working", "Working"), ("needs", "Needs you"), ("error", "Error"))
ROOT = Path(__file__).resolve().parent

ET.register_namespace("", NS)


def still_svg(markup):
    svg = ET.fromstring(markup)
    for style in svg.findall(f"{{{NS}}}style"):
        svg.remove(style)
    for attribute in ("data-animation", "data-duration", "style"):
        svg.attrib.pop(attribute)
    svg.set("class", "owl")
    svg.set("data-motion", "still")
    ET.indent(svg, space="  ")
    return ET.tostring(svg, encoding="unicode") + "\n"


def raster(page, markup, size):
    svg = ET.fromstring(markup)
    svg.set("height", str(size))
    svg.set("width", str(size))
    data = page.evaluate(
        """async ({markup, size}) => {
          const image = new Image();
          image.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(markup)}`;
          await image.decode();
          const canvas = document.createElement("canvas");
          canvas.height = canvas.width = size;
          const context = canvas.getContext("2d", {willReadFrequently: true});
          context.drawImage(image, 0, 0, size, size);
          return canvas.toDataURL("image/png").split(",")[1];
        }""",
        {"markup": ET.tostring(svg, encoding="unicode"), "size": size},
    )
    image = Image.open(io.BytesIO(base64.b64decode(data))).convert("RGBA")
    assert image.size == (size, size)
    assert image.getchannel("A").getextrema() == (0, 255), "Artwork must be visible with transparent surroundings"
    assert image.getpixel((0, 0))[3] == 0, "The transparent corner must not become a background plate"
    return image


def dib_icon(images):
    frames = []
    for size in ICO_SIZES:
        image = images[size]
        pixels = image.tobytes("raw", "BGRA", 0, -1)
        stride = ((size + 31) // 32) * 4
        mask = bytearray(stride * size)
        for y in range(size):
            for x in range(size):
                if image.getpixel((x, size - y - 1))[3] == 0:
                    mask[y * stride + x // 8] |= 1 << (7 - x % 8)
        header = struct.pack("<IIIHHIIIIII", 40, size, size * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
        frames.append(header + pixels + mask)
    entries = bytearray()
    offset = 6 + len(frames) * 16
    for size, frame in zip(ICO_SIZES, frames, strict=True):
        dimension = size if size < 256 else 0
        entries.extend(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(frame), offset))
        offset += len(frame)
    result = struct.pack("<HHH", 0, 1, len(frames)) + entries + b"".join(frames)
    decoded = Image.open(io.BytesIO(result))
    assert decoded.ico.sizes() == {(size, size) for size in ICO_SIZES}
    for size in ICO_SIZES:
        assert decoded.ico.getimage((size, size)).convert("RGBA").tobytes() == images[size].tobytes()
    return result


def preview():
    rows = []
    for theme in ("dark", "light"):
        cards = []
        for state, label in STATES:
            icons = "".join(
                f'<img alt="{label}, {size} px" height="{size}" src="owl_{state}'
                f'{"_small" if size <= 32 else ""}.svg" width="{size}">'
                for size in (128, 48, 32, 24, 20, 16)
            )
            cards.append(f"<figure><figcaption>{label}</figcaption><div>{icons}</div></figure>")
        rows.append(f'<section class="{theme}">{"".join(cards)}</section>')
    return """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>dot icons</title>
<link rel="icon" href="dot.svg" type="image/svg+xml">
<style>
  * { box-sizing: border-box; }
  body { background: #eef0f5; color: #252938; font: 16px/1.5 "Segoe UI", sans-serif; margin: 24px; }
  main { margin: auto; max-width: 1200px; }
  h1, h2 { margin: 0 0 16px; }
  header, section { border-radius: 16px; margin: 16px 0; padding: 24px; }
  header { align-items: center; display: flex; flex-wrap: wrap; gap: 32px; }
  section { display: grid; gap: 24px; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); }
  .dark { background: #19212c; color: #edf2f8; }
  .light { background: #fff; }
  figure { margin: 0; }
  figure div { align-items: center; display: flex; flex-wrap: wrap; gap: 12px; min-height: 144px; }
  figcaption { font-weight: 600; }
  img { flex: none; }
</style>
<main>
  <h1>dot icons</h1>
  <header class="dark">
    <img alt="dot" height="256" src="dot.svg" width="256">
    <img alt="dot owl" height="256" src="owl_idle.svg" width="256">
    <div><h2>Small application icon</h2>
      <img alt="dot, 16 px" height="16" src="dot_small.svg" width="16">
      <img alt="dot, 20 px" height="20" src="dot_small.svg" width="20">
      <img alt="dot, 32 px" height="32" src="dot_small.svg" width="32">
    </div>
  </header>
  ROWS
  <section class="dark">
    <figure><figcaption>Focused scan</figcaption>
      <div><img alt="Working" height="128" src="owl_working.svg" width="128"></div>
    </figure>
    <figure><figcaption>Reduced-motion still</figcaption>
      <div><img alt="Working, still" height="128" src="owl_working_still.svg" width="128">
        <img alt="Working, still, 20 px" height="20" src="owl_working_still_small.svg" width="20"></div>
    </figure>
  </section>
</main>
</html>
""".replace("ROWS", "\n  ".join(rows)).encode()


def export(check):
    outputs = {}
    sources = {}
    labels = {"dot": "dot", **{f"owl_{state}": f"dot owl: {label}" for state, label in STATES}}
    for stem, label in labels.items():
        for master, suffix in (("large", ""), ("small", "_small")):
            name = f"{stem}{suffix}"
            markup = ROOT.joinpath(f"{name}.svg").read_text(encoding="utf-8")
            svg = ET.fromstring(markup)
            assert svg.attrib["aria-label"] == label
            assert svg.attrib["data-master"] == master
            assert svg.attrib["height"] == svg.attrib["width"] == "1024"
            assert svg.attrib["viewBox"] == "0 0 1024 1024"
            if stem.startswith("owl_"):
                assert svg.attrib["data-state"] == stem.removeprefix("owl_")
                face = svg.find(f"{{{NS}}}g[@class='owl-face']")
                assert face is not None and "scale(1.5)" in face.attrib["transform"]
                assert all(node.tag in (f"{{{NS}}}defs", f"{{{NS}}}style") or node is face for node in svg)
                glints = [node for node in svg.iter(f"{{{NS}}}circle") if node.get("fill") == "#fff"]
                assert len(glints) == (2 if master == "large" else 0)
            if stem == "owl_working":
                assert svg.attrib["data-animation"] == "focused-scan"
                assert svg.attrib["data-duration"] == "1.8"
                assert re.findall(r"@keyframes\s+([\w-]+)", markup) == ["focused-scan"]
                outputs[f"owl_working_still{suffix}.svg"] = still_svg(markup).encode()
            else:
                assert not svg.findall(f"{{{NS}}}style")
            sources[name] = markup
    assert {path.stem for path in ROOT.glob("owl_*.svg")} <= {
        *sources, "owl_working_still", "owl_working_still_small"
    }

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel="msedge", headless=True)
        page = browser.new_page(viewport={"height": 1024, "width": 1024})
        for suffix in ("", "_small"):
            page.emulate_media(reduced_motion="no-preference")
            page.set_content(sources[f"owl_working{suffix}"])
            assert page.evaluate(MOTION_CHECK), "Pupils/highlights must scan downward together under fixed lids"
            page.emulate_media(reduced_motion="reduce")
            assert page.evaluate("""() => {
              const svg = document.querySelector("svg");
              return svg.getAnimations({subtree: true}).length === 0 &&
                getComputedStyle(svg.querySelector(".a-still")).display !== "none" &&
                [...svg.querySelectorAll(".a-pu")].every(node => getComputedStyle(node).transform === "none");
            }"""), "Reduced motion must show a distinct, non-animated working pose"
            page.emulate_media(reduced_motion="no-preference")
            page.set_content(outputs[f"owl_working_still{suffix}.svg"].decode())
            assert page.evaluate("""() => {
              const svg = document.querySelector("svg");
              return svg.getAnimations({subtree: true}).length === 0 &&
                getComputedStyle(svg.querySelector(".a-still")).display !== "none";
            }"""), "The standalone still must not depend on the system motion preference"
            for size in SIZES:
                if (size <= 32) != (suffix == "_small"):
                    continue
                still = raster(page, outputs[f"owl_working_still{suffix}.svg"].decode(), size)
                assert still.tobytes() != raster(page, sources[f"owl_idle{suffix}"], size).tobytes()
        app_images = {}
        for stem in labels:
            for size in SIZES:
                suffix = "_small" if size <= 32 else ""
                markup = (
                    outputs[f"owl_working_still{suffix}.svg"].decode()
                    if stem == "owl_working" else sources[f"{stem}{suffix}"]
                )
                image = raster(page, markup, size)
                assert raster(page, markup, size).tobytes() == image.tobytes(), (
                    f"Nondeterministic raster: {stem}, {size}px"
                )
                buffer = io.BytesIO()
                image.save(buffer, format="PNG")
                outputs[f"png/{stem}_{size}.png"] = buffer.getvalue()
                if stem == "dot":
                    app_images[size] = image
        browser.close()
    outputs["android/drawable-nodpi/owl_idle.png"] = outputs["png/owl_idle_256.png"]
    outputs["dot.ico"] = dib_icon(app_images)
    outputs["preview.html"] = preview()
    for name, content in sorted(outputs.items()):
        path = ROOT.joinpath(name)
        if check:
            assert path.read_bytes() == content, f"Out-of-date export: {path}"
        elif not path.is_file() or path.read_bytes() != content:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    print(
        f"{'Verified' if check else 'Exported'} {len(outputs)} artifacts; "
        "four owl states, Focused scan, alpha and DIB OK."
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify committed exports without changing files")
    export(parser.parse_args().check)
