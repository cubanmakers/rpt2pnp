/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>
#include <map>

#include "board.h"
#include "tape.h"
#include "pnp-config.h"
#include "machine.h"
#include "rpt-parser.h"
#include "rpt2pnp.h"

static const float minimum_milliseconds = 50;
static const float area_to_milliseconds = 25;  // mm^2 to milliseconds.

static int usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-l|-d|-p] <options> <rpt-file>\n"
            "Options:\n"
            "There are one of three operations to choose:\n"
            "[Operations]\n"
            "\t-l      : List found <footprint>@<component> <count> from rpt "
            "to stdout.\n"
            "\t-d      : Dispensing solder paste.\n"
            "\t-D<init-ms,area-to-ms> : Milliseconds to leave pressure on to\n"
            "\t            dispense. init-ms is initial offset, area-to-ms is\n"
            "\t            milliseconds per mm^2 area covered.\n"
            "\t-p      : Pick'n place.\n"
            "\t-P      : Output as PostScript instead of GCode.\n"
            "[Configuration]\n"
            "\t-t          : Create human-editable config template to "
            "stdout\n"
            "\t-c <config> : read such a config\n"
            "[Homer config]\n"
            "\t-H          : Create homer configuration template to stdout.\n"
            "\t-C <config> : Use homer config created via homer from -H\n",
            prog);
    return 1;
}

typedef std::map<std::string, int> ComponentCount;

// Extract components on board and their counts. Returns total components found.
int ExtractComponents(const Board::PartList& list, ComponentCount *c) {
    int total_count = 0;
    for (const Part* part : list) {
        const std::string key = part->footprint + "@" + part->value;
        (*c)[key]++;
        ++total_count;
    }
    return total_count;
}

const Part *FindPartClosestTo(const Board::PartList& list, const Position &pos) {
    const Part* result = NULL;
    float closest = -1;
    for (const Part* part : list) {
        const float dist = Distance(part->pos, pos);
        if (closest < 0 || dist < closest) {
            result = part;
            closest = dist;
        }
    }
    return result;
}

void CreateConfigTemplate(const Board& board) {
    const Board::PartList& list = board.parts();

    const float origin_x = 10, origin_y = 10;

    printf("Board:\norigin: %.0f %.0f 1.6 # x/y/z origin of the board; (z=thickness).\n\n", origin_x, origin_y);

    printf("# Where the tray with all the tapes start.\n");
    printf("Tape-Tray-Origin: 0 %.1f 0\n\n", origin_y + board.dimension().h);

    printf("# This template provides one <footprint>@<component> per tape,\n");
    printf("# but if you have multiple components that are indeed the same\n");
    printf("# e.g. smd0805@100n smd0805@0.1uF, then you can just put them\n");
    printf("# space delimited behind each Tape:\n");
    printf("#   Tape: smd0805@100n smd0805@0.1uF\n");
    printf("# Each Tape section requires\n");
    printf("#   'origin:', which is the (x/y/z) position (relative to "
           "Tape-Tray-Origin) of\n");
    printf("# the top of the first component (z: pick-up-height).\n# And\n");
    printf("#   'spacing:', (dx,dy) to the next one\n#\n");
    printf("# Also there are the following optional parameters\n");
    printf("#angle: 0     # Optional: Default rotation of component on tape.\n");
    printf("#count: 1000  # Optional: available count on tape\n");
    printf("\n");

    int ypos = 0;
    ComponentCount components;
    const int total_count = ExtractComponents(list, &components);
    for (const Part* part : list) {
        const std::string key = part->footprint + "@" + part->value;
        const auto found_count = components.find(key);
        if (found_count == components.end())
            continue; // already printed
        int width = abs(part->bounding_box.p1.x - part->bounding_box.p0.x) + 5;
        int height = abs(part->bounding_box.p1.y - part->bounding_box.p0.y);
        printf("\nTape: %s\n", key.c_str());
        printf("count: %d\n", found_count->second);
        printf("origin:  %d %d 2 # fill me\n", 10 + height/2, ypos + width/2);
        printf("spacing: %d 0   # fill me\n",
               height < 4 ? 4 : height + 2);
        ypos += width;
        components.erase(key);
    }
    fprintf(stderr, "%d components total\n", total_count);
}

void CreateList(const Board::PartList& list) {
    ComponentCount components;
    const int total_count = ExtractComponents(list, &components);
    int longest = -1;
    for (const auto &pair : components) {
        longest = std::max((int)pair.first.length(), longest);
    }
    for (const auto &pair : components) {
        printf("%-*s %4d\n", longest, pair.first.c_str(), pair.second);
    }
    fprintf(stderr, "%d components total\n", total_count);
}

void CreateHomerInstruction(const Board &board) {
    printf("bedlevel:BedLevel-Z\tTouch needle on bed next to board\n");
    ComponentCount components;
    ExtractComponents(board.parts(), &components);
    for (const auto &pair : components) {
        printf("tape%d:%s\tfind first component\n",
               1, pair.first.c_str());
        const int next_pos = std::min(std::max(2, pair.second), 4);
        printf("tape%d:%s\tfind %d. component\n",
               next_pos, pair.first.c_str(), next_pos);
    }
    const Part *board_part = FindPartClosestTo(board.parts(), Position(0, 0));
    if (board_part) {
        printf("board:%s\tfind component center on board (bottom left)\n",
               board_part->component_name.c_str());
    }
    board_part = FindPartClosestTo(board.parts(), Position(board.dimension().w,
                                                           board.dimension().h));
    if (board_part) {
        printf("board:%s\tfind component center on board (top right)\n",
               board_part->component_name.c_str());
    }
}

void SolderDispense(const Board &board, Machine *machine) {
    OptimizeList all_pads;
    for (const Part *part : board.parts()) {
        for (const Pad &pad : part->pads) {
            all_pads.push_back(std::make_pair(part, &pad));
        }
    }
    OptimizeParts(&all_pads);

    for (const auto &p : all_pads) {
        machine->Dispense(*p.first, *p.second);
    }
}

static Tape *FindTapeForPart(const PnPConfig *config, const Part *part) {
    const std::string key = part->footprint + "@" + part->value;
    auto found = config->tape_for_component.find(key);
    if (found == config->tape_for_component.end())
        return NULL;
    return found->second;
}

struct ComponentHeightComparator {
    ComponentHeightComparator(const PnPConfig *config) : config_(config) {}

    bool operator()(const Part *a, const Part *b) {
        if (a == b) return 0;
        const float a_height = GetHeight(a);
        const float b_height = GetHeight(b);
        if (a_height == b_height) {
            return a->component_name < b->component_name;
        }
        return a_height < b_height;
    }

    float GetHeight(const Part *part) {
        const Tape *tape = FindTapeForPart(config_, part);
        return tape == NULL ? -1 : tape->height();
    }
    const PnPConfig *config_;
};
void PickNPlace(const PnPConfig *config, const Board &board, Machine *machine) {
    // TODO: lowest height components first to not knock over bigger ones.
    std::vector<const Part *> list(board.parts());
    if (config) {
        std::sort(list.begin(), list.end(), ComponentHeightComparator(config));
    }
    for (const Part *part : list) {
        Tape *tape = NULL;
        if (config) {
            tape = FindTapeForPart(config, part);
            if (tape == NULL) {
                fprintf(stderr, "No tape for '%s'\n",
                        part->component_name.c_str());
            }
        }
        machine->PickPart(*part, tape);
        machine->PlacePart(*part, tape);
        if (tape) tape->Advance();
    }
}

int main(int argc, char *argv[]) {
    enum OutputType {
        OUT_NONE,
        OUT_DISPENSING,
        OUT_PICKNPLACE,
        OUT_CONFIG_TEMPLATE,
        OUT_CONFIG_LIST,
        OUT_HOMER_INSTRUCTION,
    } output_type = OUT_NONE;

    float start_ms = minimum_milliseconds;
    float area_ms = area_to_milliseconds;
    const char *config_filename = NULL;
    const char *simple_config_filename = NULL;
    bool out_postscript = false;

    int opt;
    while ((opt = getopt(argc, argv, "Pc:C:D:tlHpd")) != -1) {
        switch (opt) {
        case 'P':
            out_postscript = true;
            break;
        case 'c':
            config_filename = strdup(optarg);
            break;
        case 'C':
            simple_config_filename = strdup(optarg);
            break;
        case 'D':
            if (2 != sscanf(optarg, "%f,%f", &start_ms, &area_ms)) {
                fprintf(stderr, "Invalid -D spec\n");
                return usage(argv[0]);
            }
            break;
        case 't':
            output_type = OUT_CONFIG_TEMPLATE;
            break;
        case 'l':
            output_type = OUT_CONFIG_LIST;
            break;
        case 'H':
            output_type = OUT_HOMER_INSTRUCTION;
            break;
        case 'p':
            output_type = OUT_PICKNPLACE;
            break;
        case 'd':
            output_type = OUT_DISPENSING;
            break;
        default: /* '?' */
            return usage(argv[0]);
        }
    }

    if (optind >= argc) {
        return usage(argv[0]);
    }

    const char *rpt_file = argv[optind];

    Board board;
    if (!board.ParseFromRpt(rpt_file))
        return 1;
    fprintf(stderr, "Board: %s, %.1fmm x %.1fmm\n",
            rpt_file, board.dimension().w, board.dimension().h);

    if (output_type == OUT_CONFIG_TEMPLATE) {
        CreateConfigTemplate(board);
        return 0;
    }
    if (output_type == OUT_CONFIG_LIST) {
        CreateList(board.parts());
        return 0;
    }
    if (output_type == OUT_HOMER_INSTRUCTION) {
        CreateHomerInstruction(board);
        return 0;
    }

    PnPConfig *config = NULL;

    if (config_filename != NULL) {
        config = ParsePnPConfiguration(config_filename);
    } else if (simple_config_filename != NULL) {
        config = ParseSimplePnPConfiguration(board, simple_config_filename);
    }

    Machine *machine = out_postscript
        ? (Machine*) new PostScriptMachine()
        : (Machine*) new GCodeMachine(start_ms, area_ms);

    std::string all_args;
    for (int i = 0; i < argc; ++i) {
        all_args.append(argv[i]).append(" ");
    }
    if (!machine->Init(config, all_args, board.dimension())) {
        fprintf(stderr, "Initialization failed\n");
        return 1;
    }

    if (output_type == OUT_DISPENSING) {
        SolderDispense(board, machine);
    } else if (output_type == OUT_PICKNPLACE) {
        PickNPlace(config, board, machine);
    } else {
        fprintf(stderr, "Please choose operation with -d or -p\n");
    }

    machine->Finish();

    delete machine;
    return 0;
}
