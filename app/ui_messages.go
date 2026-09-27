package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"io/fs"
	"regexp"
	"slices"
	"strings"
	"sync"
)

//go:embed ui-locales/*.json
var uiLocaleFiles embed.FS

var uiPlaceholder = regexp.MustCompile(`\{[a-z][a-z0-9_]*\}`)

type uiTranslator struct {
	language string
	catalogs map[string]map[string]string
}

var (
	activeUIOnce sync.Once
	activeUI     uiTranslator
)

func uiText(key string) string {
	activeUIOnce.Do(func() {
		catalogs, err := readUICatalogs(uiLocaleFiles)
		if err != nil {
			panic(err) // Embedded English strings are required to show any launcher UI.
		}
		activeUI = uiTranslator{language: selectUILanguage(preferredUILanguages(), catalogs), catalogs: catalogs}
	})
	return activeUI.text(key)
}

func uiTextWith(key string, values map[string]string) string {
	return uiPlaceholder.ReplaceAllStringFunc(uiText(key), func(placeholder string) string {
		name := placeholder[1 : len(placeholder)-1]
		value, ok := values[name]
		if !ok {
			panic("missing launcher UI value: " + name)
		}
		return value
	})
}

func (t uiTranslator) text(key string) string {
	if message := t.catalogs[t.language][key]; message != "" {
		return message
	}
	if message := t.catalogs["en"][key]; message != "" {
		return message
	}
	panic("unknown launcher UI message: " + key)
}

func readUICatalogs(files fs.FS) (map[string]map[string]string, error) {
	entries, err := fs.ReadDir(files, "ui-locales")
	if err != nil {
		return nil, err
	}
	catalogs := make(map[string]map[string]string)
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		language := strings.TrimSuffix(entry.Name(), ".json")
		data, err := fs.ReadFile(files, "ui-locales/"+entry.Name())
		if err != nil {
			return nil, err
		}
		var messages map[string]string
		if err := json.Unmarshal(data, &messages); err != nil {
			return nil, fmt.Errorf("%s: %w", entry.Name(), err)
		}
		catalogs[language] = messages
	}
	english := catalogs["en"]
	if len(english) == 0 {
		return nil, fmt.Errorf("launcher UI needs a nonempty English catalog")
	}
	for key, message := range english {
		if key == "" || message == "" {
			return nil, fmt.Errorf("English launcher UI has an empty key or message")
		}
	}
	for language, messages := range catalogs {
		if language == "en" {
			continue
		}
		for key, message := range messages {
			base, exists := english[key]
			if !exists {
				return nil, fmt.Errorf("%s: unknown message %q", language, key)
			}
			if message == "" {
				continue // An unfinished translation uses English.
			}
			if !slices.Equal(sortedPlaceholders(base), sortedPlaceholders(message)) {
				return nil, fmt.Errorf("%s: placeholders differ for %q", language, key)
			}
		}
	}
	return catalogs, nil
}

func sortedPlaceholders(message string) []string {
	placeholders := uiPlaceholder.FindAllString(message, -1)
	slices.Sort(placeholders)
	return placeholders
}

func selectUILanguage(preferred []string, catalogs map[string]map[string]string) string {
	for _, language := range preferred {
		tag := strings.ToLower(strings.ReplaceAll(language, "_", "-"))
		if strings.HasPrefix(tag, "zh-") {
			variant := ""
			switch {
			case strings.Contains(tag, "hant"), strings.HasSuffix(tag, "-tw"), strings.HasSuffix(tag, "-hk"), strings.HasSuffix(tag, "-mo"):
				variant = "zh-Hant"
			case strings.Contains(tag, "hans"), strings.HasSuffix(tag, "-cn"), strings.HasSuffix(tag, "-sg"):
				variant = "zh-Hans"
			}
			if variant != "" && catalogs[variant] != nil {
				return variant
			}
		}
		for available := range catalogs {
			if strings.EqualFold(available, language) {
				return available
			}
		}
		base, _, _ := strings.Cut(tag, "-")
		if base == "zh" {
			continue // A script must be explicit; Traditional must not pick Simplified.
		}
		if catalogs[base] != nil {
			return base
		}
	}
	return "en"
}
