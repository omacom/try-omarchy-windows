# Launcher translations

[Issue #127](https://github.com/omacom/try-omarchy-windows/issues/127) tracks translation of the Windows launcher's own interface. The Linux guest already receives the Windows locale, keyboard layout, and time zone; that does not translate the launcher or Omarchy's own menus. The [Try Omarchy website](https://tryomarchy.com/) and its guides are separate translation work.

The launcher currently ships English text only. `app/ui-locales/en.json` is the source catalog. The About and launcher-update flow is the first part moved into it. Other dialogs and Settings still contain English strings. A missing translation falls back to English, and the launcher selects from Windows' preferred **UI languages**, which can differ from its regional-format setting.

To add a language, copy the English catalog to `app/ui-locales/<language-tag>.json`, then translate the values. Keep keys, placeholders such as `{version}`, product names, commands, URLs, and the actual names of untranslated Omarchy menus intact. The catalog loader rejects unknown keys and changed placeholders. Missing keys use English. Run `go test ./...` from `app/` and check the resulting windows on a real Windows machine at normal and enlarged text sizes.

Simplified Chinese (`zh-Hans`) is a candidate for the first reviewed translation. Traditional Chinese (`zh-Hant`) needs its own catalog; do not use one script as a fallback for the other. AI can draft text, but ask a fluent contributor to review installation, update, backup, reset, and removal messages before presenting a language as supported. Record which screens were reviewed and tested. The current English-only catalog does not claim Chinese UI support.
