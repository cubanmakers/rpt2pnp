/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */
#include <assert.h>

#include "tape.h"
#include <stdio.h>
#include <math.h>

Tape::Tape()
    : x_(0), y_(0), z_(0),
      dx_(0), dy_(0),
      angle_(0), slant_angle_(0),
      count_(1000) {
}

void Tape::SetFirstComponentPosition(float x, float y, float z) {
    x_ = x;
    y_ = y;
    z_ = z;
}

void Tape::SetComponentSpacing(float dx, float dy) {
    dx_ = dx;
    dy_ = dy;
    // No height difference between components (I hope :) )
    slant_angle_ = 360.0 / (2 * M_PI) * atan2f(dy, dx);
}

void Tape::SetNumberComponents(int n) {
    count_ = n;
}

bool Tape::GetPos(float *x, float *y) const {
    assert(x != NULL && y != NULL);
    if (count_ <= 0)
        return false;

    *x = x_;
    *y = y_;
    return true;
}

bool Tape::Advance() {
    if (count_ <= 0)
        return false;

    // Advance
    x_ = x_ + dx_;
    y_ = y_ + dy_;
    // z stays the same.
    --count_;

    return true;
}

void Tape::DebugPrint() const {
    fprintf(stderr, "%p: origin: (%.2f, %.2f, %.2f) delta: (%.2f,%.2f) "
            "count: %d", this, x_, y_, z_, dx_, dy_, count_);
}
