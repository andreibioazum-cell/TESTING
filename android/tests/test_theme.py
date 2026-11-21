#!/usr/bin/env python3
"""Guard text contrast in the fixed dark Obsidian Core theme."""
import pathlib
import xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parents[1] / 'app/src/main/res'
styles = ET.parse(root / 'values/styles.xml').getroot()
theme = next(style for style in styles.findall('style') if style.get('name') == 'Theme.ObsidianCore')
colors = {item.get('name'): item.text for item in theme.findall('item')}
for name in ('colorOnPrimary', 'colorOnPrimaryContainer', 'colorOnSecondary',
             'colorOnSecondaryContainer', 'colorOnTertiary', 'colorOnTertiaryContainer',
             'colorOnBackground', 'colorOnSurface', 'colorOnSurfaceVariant',
             'colorOnError', 'colorOnErrorContainer', 'android:textColorPrimary',
             'android:textColorSecondary', 'android:textColorHint'):
    assert colors[name].upper() == '#FFFFFF', name

def luminance(color):
    rgb = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in rgb]
    return .2126 * linear[0] + .7152 * linear[1] + .0722 * linear[2]

for name in ('colorPrimary', 'colorPrimaryContainer', 'colorSecondary',
             'colorSecondaryContainer', 'colorTertiary', 'colorTertiaryContainer',
             'android:colorBackground', 'colorSurface', 'colorSurfaceVariant',
             'colorSurfaceContainer', 'colorSurfaceContainerHigh', 'colorError',
             'colorErrorContainer'):
    c = colors[name].upper()
    assert len(c) == 7 and c[1:3] == c[3:5] == c[5:7], f'{name}: not neutral gray ({c})'
    ratio = 1.05 / (luminance(c) + .05)
    assert ratio >= 4.5, f'{name}: contrast {ratio:.2f} < 4.5'
print('Obsidian Core theme monochrome & white-text contrast: OK')
