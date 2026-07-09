# Publishing the Wiki

The canonical wiki source is kept in `docs/wiki/` so changes can be reviewed in
pull requests.

## Manual Publish

After merging documentation changes, publish them to the GitHub Wiki:

```sh
scripts/publish-wiki.sh
```

The script derives the wiki remote from `origin`. For this repository, it uses:

```text
https://github.com/ahmadexp/i225-NVM-FLASH.wiki.git
```

You can also pass a remote explicitly:

```sh
scripts/publish-wiki.sh git@github.com:ahmadexp/i225-NVM-FLASH.wiki.git
```

## GitHub Actions Publish

Maintainers can run the `Publish Wiki` workflow manually from the Actions tab.
The repository wiki must be enabled in GitHub settings.

