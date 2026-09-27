package main

import (
	"strings"
	"testing"
	"testing/fstest"
)

func TestEmbeddedEnglishLauncherMessages(t *testing.T) {
	catalogs, err := readUICatalogs(uiLocaleFiles)
	if err != nil {
		t.Fatal(err)
	}
	if got := selectUILanguage([]string{"zh-CN"}, catalogs); got != "en" {
		t.Fatalf("untranslated Windows language must fall back to English, got %q", got)
	}
	message := uiTextWith("about.body", map[string]string{
		"version": "v0.5.0", "website": "https://tryomarchy.com", "source": "https://github.com/omacom/try-omarchy-windows",
	})
	for _, want := range []string{"Try Omarchy v0.5.0", "Website: https://tryomarchy.com", "Source, help and issue reporting: https://github.com/omacom/try-omarchy-windows"} {
		if !strings.Contains(message, want) {
			t.Errorf("About message is missing %q", want)
		}
	}
	if uiPlaceholder.MatchString(message) {
		t.Errorf("About message has an unfilled placeholder: %q", message)
	}
	for _, key := range []string{"about.no_update", "about.update_available"} {
		message := uiTextWith(key, map[string]string{"installed": "v0.5.0", "latest": "v0.6.0"})
		if !strings.Contains(message, "v0.5.0") || !strings.Contains(message, "v0.6.0") || uiPlaceholder.MatchString(message) {
			t.Errorf("%s: broken version substitution: %q", key, message)
		}
	}
}

func TestLauncherLanguageSelectionKeepsChineseScriptsSeparate(t *testing.T) {
	catalogs := map[string]map[string]string{
		"en": {"button": "Close"}, "zh-Hans": {"button": "关闭"}, "zh-Hant": {"button": "關閉"}, "es": {"button": "Cerrar"},
	}
	for _, tc := range []struct {
		preferred []string
		want      string
	}{
		{[]string{"zh-CN"}, "zh-Hans"},
		{[]string{"zh-SG"}, "zh-Hans"},
		{[]string{"zh-TW"}, "zh-Hant"},
		{[]string{"zh-HK"}, "zh-Hant"},
		{[]string{"zh-Hant-TW"}, "zh-Hant"},
		{[]string{"es-MX"}, "es"},
		{[]string{"fr-FR", "zh-CN"}, "zh-Hans"},
		{[]string{"ja-JP"}, "en"},
	} {
		if got := selectUILanguage(tc.preferred, catalogs); got != tc.want {
			t.Errorf("%v: got %q, want %q", tc.preferred, got, tc.want)
		}
	}
	if got := selectUILanguage([]string{"zh-TW"}, map[string]map[string]string{"en": {}, "zh-Hans": {}}); got != "en" {
		t.Errorf("Traditional Chinese must not use Simplified text, got %q", got)
	}
	translator := uiTranslator{language: "zh-Hans", catalogs: map[string]map[string]string{
		"en": {"button": "Close", "help": "Help"}, "zh-Hans": {"button": "关闭"},
	}}
	if got := translator.text("button"); got != "关闭" {
		t.Errorf("translated message: %q", got)
	}
	if got := translator.text("help"); got != "Help" {
		t.Errorf("missing translation fallback: %q", got)
	}
}

func TestLauncherCatalogRejectsBrokenPlaceholders(t *testing.T) {
	files := fstest.MapFS{
		"ui-locales/en.json":      {Data: []byte(`{"prompt":"Remove {path}?"}`)},
		"ui-locales/zh-Hans.json": {Data: []byte(`{"prompt":"删除 {file}？"}`)},
	}
	if _, err := readUICatalogs(files); err == nil || !strings.Contains(err.Error(), "placeholders differ") {
		t.Fatalf("expected placeholder validation, got %v", err)
	}
	files["ui-locales/zh-Hans.json"] = &fstest.MapFile{Data: []byte(`{"prompt":"删除 {path}？","unknown":"x"}`)}
	if _, err := readUICatalogs(files); err == nil || !strings.Contains(err.Error(), "unknown message") {
		t.Fatalf("expected unknown-key validation, got %v", err)
	}
}
