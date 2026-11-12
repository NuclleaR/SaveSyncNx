"""Text helpers in savesync.c: base64, URL segments, folder names, decoding."""

import base64
from urllib.parse import quote

SPECIAL = "Pokémon™ Let's Go: #1 & 50%?"


def test_b64(run_unit):
    r = run_unit('char out[128];\n    b64_encode("me@example.com:app-pass", out, 128);\n    printf("[%s]\\n", out);')
    assert r.exit == 0, r.output
    assert "[" + base64.b64encode(b"me@example.com:app-pass").decode() + "]" in r.output


def test_b64_padding(run_unit):
    r = run_unit('char out[32];\n    b64_encode("a", out, 32);\n    printf("[%s]\\n", out);\n    b64_encode("ab", out, 32);\n    printf("[%s]\\n", out);')
    assert r.exit == 0, r.output
    assert "[YQ==]" in r.output
    assert "[YWI=]" in r.output


def test_url_add_encodes_utf8_and_specials(run_unit):
    r = run_unit(
        'char u[512];\n    strcpy(u, "http://h/x");\n'
        f'    printf("ok=%d\\n", url_add(u, "{SPECIAL}", 512));\n'
        '    printf("[%s]\\n", u);'
    )
    assert r.exit == 0, r.output
    assert "ok=1" in r.output
    assert "[http://h/x/" + quote(SPECIAL, safe="") + "]" in r.output


def test_url_add_refuses_overflow(run_unit):
    r = run_unit('char u[16];\n    strcpy(u, "http://h");\n    printf("ok=%d\\n", url_add(u, "abcdefghij", 16));\n    printf("[%s]\\n", u);')
    assert r.exit == 0, r.output
    assert "ok=0" in r.output
    assert "[http://h]" in r.output


def test_sanitize_name(run_unit):
    r = run_unit('char out[160];\n    sanitize_name("a/b\\\\c:d*e?f\\"g<h>i|j.", out, 160);\n    printf("[%s]\\n", out);')
    assert r.exit == 0, r.output
    assert "[a_b_c_d_e_f_g_h_i_j_]" in r.output


def test_sanitize_name_keeps_utf8(run_unit):
    r = run_unit('char out[160];\n    sanitize_name("0x0100ABCD00000000 Pokémon™ Let\'s Go", out, 160);\n    printf("[%s]\\n", out);')
    assert r.exit == 0, r.output
    assert "[0x0100ABCD00000000 Pokémon™ Let's Go]" in r.output


def test_xml_unescape(run_unit):
    r = run_unit('char d[128];\n    xml_unescape("a&amp;b&#39;c&#x27;d&lt;e&gt;f&quot;g&apos;h&bogus;i&#0;", d, 128);\n    printf("[%s]\\n", d);')
    assert r.exit == 0, r.output
    assert "[a&b'c'd<e>f\"g'h&bogus;i&#0;]" in r.output


def test_url_path(run_unit):
    r = run_unit(
        'char d[128];\n'
        '    url_path("https://app.koofr.net/dav/Koofr/a%20b", d, 128);\n    printf("[%s]\\n", d);\n'
        '    url_path("/dav/Koofr/x", d, 128);\n    printf("[%s]\\n", d);\n'
        '    url_path("http://h", d, 128);\n    printf("[%s]\\n", d);'
    )
    assert r.exit == 0, r.output
    assert "[/dav/Koofr/a%20b]" in r.output
    assert "[/dav/Koofr/x]" in r.output
    assert "[]" in r.output


def test_pct_decode_roundtrip(run_unit):
    r = run_unit(
        'char u[512];\n    char d[512];\n    u[0] = 0;\n'
        f'    url_add(u, "{SPECIAL}", 512);\n'
        '    pct_decode(u, d, 512);\n    printf("[%s]\\n", d);\n'
        '    pct_decode("caf%c3%a9 50% %zz", d, 512);\n    printf("[%s]\\n", d);'
    )
    assert r.exit == 0, r.output
    assert "[/" + SPECIAL + "]" in r.output
    assert "[café 50% %zz]" in r.output
