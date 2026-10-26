# dot icons

Edit `dot.svg` and the `owl_<state>.svg` masters. Their `_small.svg` counterparts are for sizes up to 32 px.
`owl_working.svg` contains Focused scan and its system reduced-motion treatment.

Export with Python, Microsoft Edge and the packages in `requirements.txt`:

```ps1
python -m pip install -r design\icons\requirements.txt
python design\icons\export_icons.py
python design\icons\export_icons.py --check
```

The exporter generates `png\`, the Android drawable in `android\`, the classic DIB `dot.ico`, working stills and
`preview.html`.
Do not edit generated files. Open `preview.html` locally to inspect the artwork on dark and light backgrounds.
Owl assets are not bundled into the desktop application.
