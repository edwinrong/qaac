#include <cwctype>
#include <shlwapi.h>
#include "expand.h"
#include "win32util.h"
#include "logging.h"
#include "reg.h"

int hex2dec(int c)
{
    switch (c) {
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    }
    return 0;
}

void RegParser::parse(const boost::shared_ptr<FILE> &fp, IRegAction *action)
{
    m_fp = fp;
    m_action = action;
    m_lineno = 1;
    int c = version();
    c = newline(c);
    while (c != WEOF) {
	c = get();
	if (c == '[')
	    c = key(get());
	else if (c == '\r' || c == '\n')
	    c = newline(c);
	else if (c == '@' || c == '"')
	    c = value(c);
	else if (c != WEOF)
	    error(format("Illegal entry found: 0x%02x", c & 0xff));
    }
}

int RegParser::skipws()
{
    int c;
    while ((c = get()) != WEOF && std::iswspace(c))
	;
    return c;
}

int RegParser::hexDigits(int c, int width)
{
    int nc = 0;
    if (std::iswxdigit(c)) {
	do {
	    put(c);
	    ++nc;
	} while ((c = get()) != WEOF && std::iswxdigit(c));
    }
    if (nc == 0) error("Hex digits expected");
    if (width && nc != width) error("Invalid number of hex digits");
    return c;
}

std::vector<BYTE> RegParser::getRawValue()
{
    std::vector<BYTE> value;
    char buf[3] = { 0 };
    for (size_t i = 0; i < m_token.size() / 2; ++i) {
	int n = (hex2dec(m_token[i*2]) << 4) | hex2dec(m_token[i*2+1]);
	value.push_back(n);
    }
    return value;
}

int RegParser::version()
{
    wint_t c;
    while ((c = get()) != WEOF && c != '\r' && c != '\n')
	;
    return newline(c);
}

int RegParser::key(int c)
{
    do {
	if (c != ']') {
	    put(c);
	    c = get();
	} else {
	    c = get();
	    if (c == '\r' || c == '\n') {
		onKey();
		return newline(c);
	    }
	    put(']');
	}
    } while (c != WEOF);
    return c;
}

int RegParser::value(int c)
{
    if (c != '@')
	c = valueName(get());
    else {
	put(c);
	onValueName();
	expect('=');
    }
    return valueData(get());
}

int RegParser::valueName(int c)
{
    do {
	if (c == '\\')
	    c = get();
	else if (c == '"') {
	    onValueName();
	    return expect('=');
	}
	put(c);
    } while ((c = get()) != WEOF);
    return c;
}

int RegParser::valueData(int c)
{
    if (c == '"') {
	m_action->onType(REG_SZ);
	return stringValue(c);
    } else if (c == 'h') {
	c = hexType(c);
	return hexValue(get());
    }
    else if (c == 'd')
	return dwordValue(c);
    else if (c == 'e')
	return evalValue(c);
    else
	error("Invalid value data");
}

int RegParser::stringValue(int c)
{
    while ((c = get()) != WEOF) {
	if (c == '\\')
	    c = get();
	else if (c == '"') {
	    onStringValue();
	    return newline(get());
	}
	put(c);
    }
    return c;
}

int RegParser::hexType(int c)
{
    int value = 0;
    expect('e'); expect('x');
    c = get();
    if (c == '(') {
	if (hexDigits(get()) != ')') error(") is expected");
	onHexType();
	c = expect(':');
    } else if (c == ':')
	m_action->onType(REG_BINARY);
    else
	error("Invalid hex type decl");
    return c;
}

int RegParser::hexValue(int c)
{
    do {
	c = hexDigits(c, 2);
	if (c == ',') {
	    c = get();
	    if (c == '\\') c = skipws();
	}
    } while (isxdigit(c));
    onHexValue();
    return newline(c);
}

int RegParser::dwordValue(int c)
{
    expect('w'); expect('o'); expect('r'); expect('d'); expect(':');
    c = hexDigits(get(), 8);
    onDwordValue();
    return newline(c);
}

int RegParser::evalValue(int c)
{
    expect('v'); expect('a'); expect('l'); expect(':'); expect('"');
    while ((c = get()) != WEOF) {
	if (c == '\\')
	    c = get();
	else if (c == '"') {
	    onEvalValue();
	    return newline(get());
	}
	put(c);
    }
    return c;
}

static void cleanup()
{
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, 0);
    SHDeleteKeyW(HKEY_CURRENT_USER, L"SOFTWARE\\qaac");
}

void RegAction::realize()
{
    HKEY rootKey;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\qaac", 0, 0,
	    REG_OPTION_VOLATILE, KEY_ALL_ACCESS, 0, &rootKey, 0);
    boost::shared_ptr<HKEY__> __rootKey__(rootKey, RegCloseKey);
    hive_t::const_iterator ii;
    section_t::const_iterator jj;
    for (ii = m_entries.begin(); ii != m_entries.end(); ++ii) {
	std::vector<wchar_t> keybuf(ii->first.size() + 1);
	std::wcscpy(&keybuf[0], ii->first.c_str());
	wchar_t *root, *rest = &keybuf[0];
	root = wcssep(&rest, L"\\");
	if (!root || !rest) continue;
	HKEY hKey;
	RegCreateKeyExW(rootKey, rest, 0, 0, REG_OPTION_VOLATILE,
		KEY_ALL_ACCESS, 0, &hKey, 0);
	boost::shared_ptr<HKEY__> __hKey__(hKey, RegCloseKey);
	for (jj = ii->second.begin(); jj != ii->second.end(); ++jj)
	    RegSetValueExW(hKey, jj->first.c_str(), 0, jj->second.type,
		    &jj->second.value[0], jj->second.value.size());
    }
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, rootKey);
    std::atexit(cleanup);
}

void RegAction::show()
{
    hive_t::const_iterator ii;
    section_t::const_iterator jj;
    LOG("==== Registry setting start ====\n");
    for (ii = m_entries.begin(); ii != m_entries.end(); ++ii) {
	LOG("KEY: [%ls]\n", ii->first.c_str());
	for (jj = ii->second.begin(); jj != ii->second.end(); ++jj) {
	    const std::vector<BYTE> &vec = jj->second.value;
	    if (jj->second.type == REG_SZ)
		LOG("  VALUE: [%ls]=[%ls]\n", jj->first.c_str(), &vec[0]);
	    else {
		LOG("  VALUE: [%ls][0x%x]=",
			jj->first.c_str(), jj->second.type);
		std::vector<BYTE>::const_iterator kk;
		for (kk = vec.begin(); kk != vec.end(); ++kk)
		    LOG("%02x ", *kk & 0xff);
		LOG("\n");
	    }
	}
    }
    LOG("==== Registry setting end ====\n");
}
