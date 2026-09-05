package main

import (
	"regexp"
	"strings"
)

// windowsZoneToIANA maps Windows time zone key names to the IANA zone the
// guest can use, from CLDR windowsZones.xml (territory 001, first zone).
// Regenerate from https://github.com/unicode-org/cldr/blob/main/common/supplemental/windowsZones.xml
// when Windows adds zones.
var windowsZoneToIANA = map[string]string{
	"AUS Central Standard Time":       "Australia/Darwin",
	"AUS Eastern Standard Time":       "Australia/Sydney",
	"Afghanistan Standard Time":       "Asia/Kabul",
	"Alaskan Standard Time":           "America/Anchorage",
	"Aleutian Standard Time":          "America/Adak",
	"Altai Standard Time":             "Asia/Barnaul",
	"Arab Standard Time":              "Asia/Riyadh",
	"Arabian Standard Time":           "Asia/Dubai",
	"Arabic Standard Time":            "Asia/Baghdad",
	"Argentina Standard Time":         "America/Buenos_Aires",
	"Astrakhan Standard Time":         "Europe/Astrakhan",
	"Atlantic Standard Time":          "America/Halifax",
	"Aus Central W. Standard Time":    "Australia/Eucla",
	"Azerbaijan Standard Time":        "Asia/Baku",
	"Azores Standard Time":            "Atlantic/Azores",
	"Bahia Standard Time":             "America/Bahia",
	"Bangladesh Standard Time":        "Asia/Dhaka",
	"Belarus Standard Time":           "Europe/Minsk",
	"Bougainville Standard Time":      "Pacific/Bougainville",
	"Canada Central Standard Time":    "America/Regina",
	"Cape Verde Standard Time":        "Atlantic/Cape_Verde",
	"Caucasus Standard Time":          "Asia/Yerevan",
	"Cen. Australia Standard Time":    "Australia/Adelaide",
	"Central America Standard Time":   "America/Guatemala",
	"Central Asia Standard Time":      "Asia/Bishkek",
	"Central Brazilian Standard Time": "America/Cuiaba",
	"Central Europe Standard Time":    "Europe/Budapest",
	"Central European Standard Time":  "Europe/Warsaw",
	"Central Pacific Standard Time":   "Pacific/Guadalcanal",
	"Central Standard Time":           "America/Chicago",
	"Central Standard Time (Mexico)":  "America/Mexico_City",
	"Chatham Islands Standard Time":   "Pacific/Chatham",
	"China Standard Time":             "Asia/Shanghai",
	"Cuba Standard Time":              "America/Havana",
	"Dateline Standard Time":          "Etc/GMT+12",
	"E. Africa Standard Time":         "Africa/Nairobi",
	"E. Australia Standard Time":      "Australia/Brisbane",
	"E. Europe Standard Time":         "Europe/Chisinau",
	"E. South America Standard Time":  "America/Sao_Paulo",
	"Easter Island Standard Time":     "Pacific/Easter",
	"Eastern Standard Time":           "America/New_York",
	"Eastern Standard Time (Mexico)":  "America/Cancun",
	"Egypt Standard Time":             "Africa/Cairo",
	"Ekaterinburg Standard Time":      "Asia/Yekaterinburg",
	"FLE Standard Time":               "Europe/Kiev",
	"Fiji Standard Time":              "Pacific/Fiji",
	"GMT Standard Time":               "Europe/London",
	"GTB Standard Time":               "Europe/Bucharest",
	"Georgian Standard Time":          "Asia/Tbilisi",
	"Greenland Standard Time":         "America/Godthab",
	"Greenwich Standard Time":         "Atlantic/Reykjavik",
	"Haiti Standard Time":             "America/Port-au-Prince",
	"Hawaiian Standard Time":          "Pacific/Honolulu",
	"India Standard Time":             "Asia/Calcutta",
	"Iran Standard Time":              "Asia/Tehran",
	"Israel Standard Time":            "Asia/Jerusalem",
	"Jordan Standard Time":            "Asia/Amman",
	"Kaliningrad Standard Time":       "Europe/Kaliningrad",
	"Korea Standard Time":             "Asia/Seoul",
	"Libya Standard Time":             "Africa/Tripoli",
	"Line Islands Standard Time":      "Pacific/Kiritimati",
	"Lord Howe Standard Time":         "Australia/Lord_Howe",
	"Magadan Standard Time":           "Asia/Magadan",
	"Magallanes Standard Time":        "America/Punta_Arenas",
	"Marquesas Standard Time":         "Pacific/Marquesas",
	"Mauritius Standard Time":         "Indian/Mauritius",
	"Middle East Standard Time":       "Asia/Beirut",
	"Montevideo Standard Time":        "America/Montevideo",
	"Morocco Standard Time":           "Africa/Casablanca",
	"Mountain Standard Time":          "America/Denver",
	"Mountain Standard Time (Mexico)": "America/Mazatlan",
	"Myanmar Standard Time":           "Asia/Rangoon",
	"N. Central Asia Standard Time":   "Asia/Novosibirsk",
	"Namibia Standard Time":           "Africa/Windhoek",
	"Nepal Standard Time":             "Asia/Katmandu",
	"New Zealand Standard Time":       "Pacific/Auckland",
	"Newfoundland Standard Time":      "America/St_Johns",
	"Norfolk Standard Time":           "Pacific/Norfolk",
	"North Asia East Standard Time":   "Asia/Irkutsk",
	"North Asia Standard Time":        "Asia/Krasnoyarsk",
	"North Korea Standard Time":       "Asia/Pyongyang",
	"Omsk Standard Time":              "Asia/Omsk",
	"Pacific SA Standard Time":        "America/Santiago",
	"Pacific Standard Time":           "America/Los_Angeles",
	"Pacific Standard Time (Mexico)":  "America/Tijuana",
	"Pakistan Standard Time":          "Asia/Karachi",
	"Paraguay Standard Time":          "America/Asuncion",
	"Qyzylorda Standard Time":         "Asia/Qyzylorda",
	"Romance Standard Time":           "Europe/Paris",
	"Russia Time Zone 10":             "Asia/Srednekolymsk",
	"Russia Time Zone 11":             "Asia/Kamchatka",
	"Russia Time Zone 3":              "Europe/Samara",
	"Russian Standard Time":           "Europe/Moscow",
	"SA Eastern Standard Time":        "America/Cayenne",
	"SA Pacific Standard Time":        "America/Bogota",
	"SA Western Standard Time":        "America/La_Paz",
	"SE Asia Standard Time":           "Asia/Bangkok",
	"Saint Pierre Standard Time":      "America/Miquelon",
	"Sakhalin Standard Time":          "Asia/Sakhalin",
	"Samoa Standard Time":             "Pacific/Apia",
	"Sao Tome Standard Time":          "Africa/Sao_Tome",
	"Saratov Standard Time":           "Europe/Saratov",
	"Singapore Standard Time":         "Asia/Singapore",
	"South Africa Standard Time":      "Africa/Johannesburg",
	"South Sudan Standard Time":       "Africa/Juba",
	"Sri Lanka Standard Time":         "Asia/Colombo",
	"Sudan Standard Time":             "Africa/Khartoum",
	"Syria Standard Time":             "Asia/Damascus",
	"Taipei Standard Time":            "Asia/Taipei",
	"Tasmania Standard Time":          "Australia/Hobart",
	"Tocantins Standard Time":         "America/Araguaina",
	"Tokyo Standard Time":             "Asia/Tokyo",
	"Tomsk Standard Time":             "Asia/Tomsk",
	"Tonga Standard Time":             "Pacific/Tongatapu",
	"Transbaikal Standard Time":       "Asia/Chita",
	"Turkey Standard Time":            "Europe/Istanbul",
	"Turks And Caicos Standard Time":  "America/Grand_Turk",
	"US Eastern Standard Time":        "America/Indianapolis",
	"US Mountain Standard Time":       "America/Phoenix",
	"UTC":                             "Etc/UTC",
	"UTC+12":                          "Etc/GMT-12",
	"UTC+13":                          "Etc/GMT-13",
	"UTC-02":                          "Etc/GMT+2",
	"UTC-08":                          "Etc/GMT+8",
	"UTC-09":                          "Etc/GMT+9",
	"UTC-11":                          "Etc/GMT+11",
	"Ulaanbaatar Standard Time":       "Asia/Ulaanbaatar",
	"Venezuela Standard Time":         "America/Caracas",
	"Vladivostok Standard Time":       "Asia/Vladivostok",
	"Volgograd Standard Time":         "Europe/Volgograd",
	"W. Australia Standard Time":      "Australia/Perth",
	"W. Central Africa Standard Time": "Africa/Lagos",
	"W. Europe Standard Time":         "Europe/Berlin",
	"W. Mongolia Standard Time":       "Asia/Hovd",
	"West Asia Standard Time":         "Asia/Tashkent",
	"West Bank Standard Time":         "Asia/Hebron",
	"West Pacific Standard Time":      "Pacific/Port_Moresby",
	"Yakutsk Standard Time":           "Asia/Yakutsk",
	"Yukon Standard Time":             "America/Whitehorse",
}

// keyboardLayoutForKLID maps a Windows keyboard layout identifier (the
// eight-digit KLID from the user's Preload list) to an XKB layout and
// variant. Full identifiers take precedence over the language in the low
// sixteen bits, so US-International and Dvorak are told apart from plain US.
var keyboardLayoutForKLID = map[string][2]string{
	"00010409": {"us", "dvorak"},
	"00020409": {"us", "intl"},
	"00030409": {"us", "dvorak-l"},
	"00040409": {"us", "dvorak-r"},
	"00050409": {"us", "dvp"},
	"00000452": {"gb", ""},
	"00011809": {"ie", ""},
	"0001080c": {"be", ""},
	"00010407": {"de", "T3"},
	"0001040c": {"fr", "bepo"},
	"0001041b": {"sk", "qwerty"},
	"00010405": {"cz", "qwerty"},
	"00010415": {"pl", "qwertz"},
	"00010419": {"ru", "typewriter"},
}

var keyboardLayoutForLanguage = map[string][2]string{
	"0409": {"us", ""}, "0809": {"gb", ""}, "0c09": {"us", ""}, "1009": {"us", ""}, "1409": {"us", ""},
	"1809": {"ie", ""}, "1c09": {"za", ""}, "4009": {"in", "eng"},
	"0407": {"de", ""}, "0807": {"ch", ""}, "0c07": {"at", ""},
	"040c": {"fr", ""}, "080c": {"be", ""}, "0c0c": {"ca", ""}, "100c": {"ch", "fr"}, "140c": {"lu", ""},
	"0410": {"it", ""}, "0810": {"ch", "it"},
	"0c0a": {"es", ""}, "040a": {"es", ""}, "080a": {"latam", ""}, "100a": {"latam", ""}, "180a": {"latam", ""},
	"200a": {"latam", ""}, "240a": {"latam", ""}, "280a": {"latam", ""}, "2c0a": {"latam", ""}, "300a": {"latam", ""},
	"340a": {"latam", ""}, "380a": {"latam", ""}, "3c0a": {"latam", ""}, "400a": {"latam", ""}, "440a": {"latam", ""},
	"480a": {"latam", ""}, "4c0a": {"latam", ""}, "500a": {"latam", ""},
	"0416": {"br", ""}, "0816": {"pt", ""},
	"0413": {"us", "intl"}, "0813": {"be", ""},
	"041d": {"se", ""}, "0414": {"no", ""}, "0814": {"no", ""}, "0406": {"dk", ""}, "040b": {"fi", ""}, "040f": {"is", ""},
	"0415": {"pl", ""}, "0405": {"cz", ""}, "041b": {"sk", ""}, "040e": {"hu", ""}, "0418": {"ro", ""},
	"0424": {"si", ""}, "041a": {"hr", ""}, "081a": {"rs", "latin"}, "0c1a": {"rs", ""}, "141a": {"ba", ""}, "042f": {"mk", ""},
	"0402": {"bg", ""}, "0419": {"ru", ""}, "0422": {"ua", ""}, "0423": {"by", ""},
	"0425": {"ee", ""}, "0426": {"lv", ""}, "0427": {"lt", ""},
	"0408": {"gr", ""}, "041f": {"tr", ""}, "042c": {"az", ""}, "0437": {"ge", ""}, "042b": {"am", ""},
	"040d": {"il", ""}, "0401": {"ara", ""}, "0429": {"ir", ""}, "0420": {"pk", ""}, "0439": {"in", ""},
	"041e": {"th", ""}, "042a": {"vn", ""}, "0411": {"jp", ""}, "0412": {"kr", ""},
	"0804": {"cn", ""}, "0404": {"tw", ""}, "0c04": {"cn", ""}, "1004": {"cn", ""}, "1404": {"cn", ""},
	"043f": {"kz", ""}, "0443": {"uz", ""}, "0440": {"kg", ""}, "0450": {"mn", ""},
}

var (
	validZoneName    = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_+./-]{0,63}$`)
	validLayoutName  = regexp.MustCompile(`^[a-z]{2,8}$`)
	validVariantName = regexp.MustCompile(`^[A-Za-z0-9_-]{0,32}$`)
)

// ianaZoneForWindows returns the IANA zone for a Windows time zone key name,
// or "" when the name is unknown.
func ianaZoneForWindows(key string) string {
	zone := windowsZoneToIANA[strings.TrimSpace(key)]
	if !validZoneName.MatchString(zone) {
		return ""
	}
	return zone
}

// xkbForKLID returns the XKB layout and variant for a Windows keyboard layout
// identifier, or "" when the identifier is unknown.
func xkbForKLID(klid string) (string, string) {
	klid = strings.ToLower(strings.TrimSpace(klid))
	if len(klid) != 8 {
		return "", ""
	}
	if m, ok := keyboardLayoutForKLID[klid]; ok {
		return m[0], m[1]
	}
	if m, ok := keyboardLayoutForLanguage[klid[4:]]; ok {
		return m[0], m[1]
	}
	return "", ""
}

// hostLocaleCmdline builds the kernel parameters that tell the guest which
// time zone and keyboard layout Windows uses. Unknown values add nothing, so
// the guest keeps whatever it has.
func hostLocaleCmdline(zone, layout, variant string) string {
	words := ""
	if zone != "" && validZoneName.MatchString(zone) {
		words += " tryomarchy.tz=" + zone
	}
	if layout != "" && validLayoutName.MatchString(layout) && validVariantName.MatchString(variant) {
		words += " tryomarchy.kb=" + layout
		if variant != "" {
			words += ":" + variant
		}
	}
	return words
}

// splitKeyboardSpec parses "layout" or "layout:variant" as given on the
// command line or in settings.
func splitKeyboardSpec(spec string) (string, string) {
	layout, variant, _ := strings.Cut(strings.TrimSpace(spec), ":")
	return layout, variant
}
