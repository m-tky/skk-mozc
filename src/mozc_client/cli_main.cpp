/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone CLI for sanity-checking the IPC client during development.
 *
 *   mozc-client-cli あさひしんぶん
 *
 * Output is one candidate per line: "<value>\t<cost>\t<description>".
 */

#include "mozc_client.h"
#include "../log/log.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <yomi>\n", argv[0]);
        return 2;
    }
    std::string yomi = argv[1];

    skk_mozc::MozcClientOptions opts;
    opts.debug = true;
    // Use the same env knobs the fcitx5 addon does so behavior matches.
    if (const char *p = std::getenv("SKK_MOZC_MOZC_SERVER"); p && *p) {
        opts.mozc_server_path = p;
    } else if (const char *p = std::getenv("MOZC_SERVER"); p && *p) {
        opts.mozc_server_path = p;
    }
    if (const char *d = std::getenv("SKK_MOZC_DEBUG"); d && *d &&
        std::string(d) != "0") {
        skk_mozc::log::setEnabled(true);
    }

    skk_mozc::MozcClient client(opts);
    auto out = client.convert(yomi);
    if (!out) {
        std::fprintf(stderr, "mozc conversion failed or timed out\n");
        return 1;
    }
    for (const auto &c : out->top_candidates) {
        std::printf("%s\t%d\t%s\n", c.value.c_str(), c.cost,
                    c.description.c_str());
    }
    return 0;
}
