/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include "pnp-config.h"

#include <stdio.h>
#include <math.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdio.h>

#include "tape.h"
#include "board.h"

#define TYPICAL_BOARD_THICKNESS 1.6

PnPConfig *ParsePnPConfiguration(const std::string& filename) {
    std::unique_ptr<PnPConfig> result(new PnPConfig());

    // TODO: this parsing is very simplistic.
    // TODO: and ugly with c++ streams.

    std::string token;
    float x, y, z;
    Tape* current_tape = NULL;
    int line = 1;
    Position tape_tray_origin;
    float tape_tray_height = 0.0f;

    std::ifstream in(filename);
    while (!in.eof()) {
        token.clear();
        in >> token;

        char buffer[1024];
        in.getline(buffer, sizeof(buffer), '\n');
        ++line;

        if (token.empty() || token[0] == '#')
            continue;

        if (token == "Board:") {
            if (current_tape) current_tape = NULL;
        } else if (token == "Tape-Tray-Origin:") {
            if (current_tape) current_tape = NULL;
            if (2 > sscanf(buffer, "%f %f %f",
                           &tape_tray_origin.x,
                           &tape_tray_origin.y,
                           &tape_tray_height)) {
                fprintf(stderr, "%s:%d: Parse problem tape-tray origin: '%s'\n",
                        filename.c_str(), line, buffer);
                return NULL;
            }
        } else if (token == "Tape:") {
            current_tape = new Tape();
            current_tape->SetAngle(90);
            // This tape is valid for multiple values/footprints possibly.
            // Lets all parse them
            token.clear();
            std::string all_the_names = buffer;
            std::stringstream parts(all_the_names);
            while (!parts.eof()) {
                parts >> token;
                result->tape_for_component[token] = current_tape;
            }
        } else if (token == "origin:") {
            if (current_tape) {
                if (3 != sscanf(buffer, "%f %f %f", &x, &y, &z)) {
                    fprintf(stderr, "%s:%d: Parse problem tape origin: '%s'\n",
                            filename.c_str(), line, buffer);
                    return NULL;
                }
                current_tape->SetFirstComponentPosition(
                    x + tape_tray_origin.x,
                    y + tape_tray_origin.y,
                    z + tape_tray_height);
            } else {
                if (2 > sscanf(buffer, "%f %f %f",
                               &result->board.origin.x,
                               &result->board.origin.y,
                               &result->board.top)) {  // optional top
                    fprintf(stderr, "%s:%d: Parse problem board origin: '%s'\n",
                            filename.c_str(), line, buffer);
                    return NULL;
                }
            }
        } else if (token == "spacing:") {
            if (!current_tape) {
                std::cerr << filename << ":" << line << "spacing without tape";
                return NULL;
            }
            if (2 != sscanf(buffer, "%f %f", &x, &y)) {
                fprintf(stderr, "%s:%d: Parse problem spacing: '%s'\n",
                        filename.c_str(), line, buffer);
                return NULL;
            }
            if (x == 0 && y == 0) {
                fprintf(stderr, "%s:%d: "
                        "Spacing: eat least one needs to be set '%s'\n",
                        filename.c_str(), line, buffer);  // line no ?
                return NULL;
            }
            current_tape->SetComponentSpacing(x, y);
        } else if (token == "angle:") {
            if (!current_tape) {
                std::cerr << "spacing without tape";
                return NULL;
            }
            if (1 != sscanf(buffer, "%f", &x)) {
                fprintf(stderr, "%s:%d: Parse problem angle: '%s'\n",
                        filename.c_str(), line, buffer);
                return NULL;
            }
            current_tape->SetAngle(x);
        } else if (token == "count:") {
            if (!current_tape) {
                std::cerr << "Count without tape.";
                return NULL;
            }
            int count;
            if (1 != sscanf(buffer, "%d", &count)) {
                fprintf(stderr, "%s:%d: Parse problem count: '%s'.\n",
                        filename.c_str(), line, buffer);
                return NULL;
            }
            current_tape->SetNumberComponents(count);
        } else {
            fprintf(stderr, "%s:%d: invalid token '%s'\n",
                    filename.c_str(), line, token.c_str());
            return NULL;
        }
    }

    // Let's assume that for now
    result->bed_level = 0;

    return result.release();
}

static bool FindPartPos(const Board &board, const char *part_name,
                        Position *pos) {
    for (const Part* part : board.parts()) {
        if (part->component_name == part_name) {
            *pos = part->pos;
            return true;
        }
    }
    return false;
}

PnPConfig *ParseSimplePnPConfiguration(const Board &board,
                                       const std::string& filename) {
    std::unique_ptr<PnPConfig> result(new PnPConfig());

    FILE *in = fopen(filename.c_str(), "r");
    if (!in) {
        fprintf(stderr, "Can't open %s\n", filename.c_str());
        return NULL;
    }
    char buffer[1024];
    float x, y, z;
    int tape_idx;
    char designator[256];
    while (fgets(buffer, sizeof(buffer), in)) {
        if (5 == sscanf(buffer, "tape%d:%s %f %f %f\n", &tape_idx, designator,
                        &x, &y, &z)) {
            if (tape_idx == 1) {
                Tape *t = new Tape();

                // TODO(hzeller): make configurable. Empirically, this is true
                // for the 0805 components we have been playing with.
                // (really it is the relative angle to what people drew in
                // KiCad component vs. how it is on tape).
                t->SetAngle(90);

                t->SetFirstComponentPosition(x, y, z);
                result->tape_for_component[designator] = t;
            } else {
                PnPConfig::PartToTapeMap::iterator found;
                found = result->tape_for_component.find(designator);
                if (found != result->tape_for_component.end()) {
                    Tape *t = found->second;
                    const int advance = tape_idx - 1;
                    float old_x, old_y;
                    t->GetPos(&old_x, &old_y);
                    const float dx = (x - old_x) / advance;
                    const float dy = (y - old_y) / advance;
                    found->second->SetComponentSpacing(dx, dy);
                    fprintf(stderr, "Δ=%.2fmm ∡=%5.1f° %s\n",
                            sqrt(dx*dx + dy*dy), found->second->angle(),
                            designator);
                }
            }
        } else if (4 == sscanf(buffer, "board:%s %f %f %f\n", designator,
                               &x, &y, &z)) {
            Position part_pos;
            if (FindPartPos(board, designator, &part_pos)) {
                result->board.origin.x = x - part_pos.x;
                result->board.origin.y = y - part_pos.y;
            } else {
                fprintf(stderr, "Trouble finding '%s'\n", designator);
            }
            result->board.top = z;
            if (result->bed_level < 0) {
                result->bed_level = result->board.top - TYPICAL_BOARD_THICKNESS;
            }
        } else if (4 == sscanf(buffer, "bedlevel:%s %f %f %f\n", designator,
                               &x, &y, &z)) {
            result->bed_level = z;
        } else {
            fprintf(stderr, "Couldn't parse '%s'\n", buffer);
        }
    }

    // Cross check
    float lowest_value = result->board.top;
    for (const auto &t : result->tape_for_component) {
        lowest_value = std::min(lowest_value, t.second->height());
    }
    if (lowest_value < result->bed_level) {
        fprintf(stderr, "Mmh, looks like there are things _below_ bed-level?\n"
                "I'd consider this an error ... "
                "(bed-level=%.1f, lowest point=%.1f\n",
                result->bed_level, lowest_value);
        return NULL;
    }
    return result.release();
}

PnPConfig *CreateEmptyConfiguration() {
    std::unique_ptr<PnPConfig> result(new PnPConfig());
    result->board.origin = { 0, 0 };
    result->board.top = TYPICAL_BOARD_THICKNESS;
    result->bed_level = 0;
    return result.release();
}
