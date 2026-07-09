#!/usr/bin/env sh
set -eu

repo_root=$(git rev-parse --show-toplevel)
wiki_src="$repo_root/docs/wiki"

if [ ! -d "$wiki_src" ]; then
  echo "docs/wiki does not exist" >&2
  exit 1
fi

remote="${1:-${WIKI_REMOTE:-}}"
if [ -z "$remote" ]; then
  origin=$(git -C "$repo_root" config --get remote.origin.url)
  case "$origin" in
    *.wiki.git) remote="$origin" ;;
    *.git) remote="${origin%.git}.wiki.git" ;;
    *) remote="${origin}.wiki.git" ;;
  esac
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT HUP TERM

wiki_dir="$tmp/wiki"
if git ls-remote "$remote" HEAD >/dev/null 2>&1; then
  git clone "$remote" "$wiki_dir"
else
  mkdir "$wiki_dir"
  git -C "$wiki_dir" init
  git -C "$wiki_dir" remote add origin "$remote"
fi

find "$wiki_dir" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -R "$wiki_src"/. "$wiki_dir"/

git -C "$wiki_dir" add -A
if git -C "$wiki_dir" diff --cached --quiet; then
  echo "Wiki already up to date."
  exit 0
fi

if ! git -C "$wiki_dir" config user.email >/dev/null; then
  git -C "$wiki_dir" config user.email "actions@github.com"
fi
if ! git -C "$wiki_dir" config user.name >/dev/null; then
  git -C "$wiki_dir" config user.name "GitHub Actions"
fi

git -C "$wiki_dir" commit -m "Update wiki from docs/wiki"
git -C "$wiki_dir" push origin HEAD:master

