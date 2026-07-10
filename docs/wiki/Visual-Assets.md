# Visual Assets

This repository keeps its public graphics in `assets/`.

## GitHub Social Preview

The GitHub repository social preview image is:

```text
assets/github-social-preview.png
```

It is rendered at `1280x640`, the recommended 2:1 social card size.

![GitHub social preview](https://raw.githubusercontent.com/ahmadexp/i225-NVM-FLASH/main/assets/github-social-preview.png)

The editable SVG source is:

```text
assets/github-social-preview.svg
```

## Stamp Logo

The stamp logo is available as both SVG and PNG:

```text
assets/i225-nvm-flash-4-pi-stamp.svg
assets/i225-nvm-flash-4-pi-stamp.png
```

![Stamp logo](https://raw.githubusercontent.com/ahmadexp/i225-NVM-FLASH/main/assets/i225-nvm-flash-4-pi-stamp.png)

## Regenerating PNGs

The checked-in PNGs are rendered from the SVG sources:

```sh
magick -background none assets/i225-nvm-flash-4-pi-stamp.svg \
  assets/i225-nvm-flash-4-pi-stamp.png

magick -background none assets/github-social-preview.svg \
  assets/github-social-preview.png
```

