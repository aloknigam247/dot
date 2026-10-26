# dot icons

Edit `dot.svg` and the `owl_<state>.svg` masters. Their `_small.svg` counterparts are for sizes up to 32 px.
Working is SVG-only; `owl_working.svg` contains Focused scan, paused for system reduced motion.

Export with Python, Microsoft Edge and the packages in `requirements.txt`:

```ps1
python -m pip install -r design\icons\requirements.txt
python design\icons\export_icons.py
python design\icons\export_icons.py --check
```

The exporter generates `png\`, the Android drawable in `android\`, and the classic DIB `dot.ico`.
Do not edit generated files.
Owl assets are not bundled into the desktop application.
