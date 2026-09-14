/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#pragma once

#include <cassert>
#include <cstdint>

#include <Inventor/SbBSPTree.h>
#include <Inventor/SbLinear.h>
#include <Inventor/SbName.h>
#include <Inventor/SbString.h>

// NOTE: Coin's SoVectorizeAction forward-declares SoVectorizeItem in the global
// namespace, so the item classes below must not be put into a namespace.

class SoVectorizeItem
{
public:
    SoVectorizeItem()
    {
        this->type = UNDEFINED;
        this->depth = 0.0f;
    }
    // quick and easy type system
    enum Type
    {
        UNDEFINED,
        LINE,
        TRIANGLE,
        TEXT,
        POINT,
        IMAGE
    };
    int type;
    float depth;  // for depth sorting
};

class SoVectorizePoint: public SoVectorizeItem
{
public:
    SoVectorizePoint()
    {
        this->type = POINT;
        this->vidx = 0;
        this->size = 1.0f;
        this->col = 0;
    }
    int vidx;    // index to BSPtree coordinate
    float size;  // Coin size (pixels)
    uint32_t col;
};

class SoVectorizeTriangle: public SoVectorizeItem
{
public:
    SoVectorizeTriangle()
    {
        this->type = TRIANGLE;
    }
    int vidx[3];  // indices to BSPtree coordinates
    uint32_t col[3];
};

class SoVectorizeLine: public SoVectorizeItem
{
public:
    SoVectorizeLine()
    {
        this->type = LINE;
        vidx[0] = 0;
        vidx[1] = 0;
        col[0] = 0;
        col[1] = 0;
        this->pattern = 0xffff;
        this->width = 1.0f;
    }
    int vidx[2];  // indices to BSPtree coordinates
    uint32_t col[2];
    uint16_t pattern;  // Coin line pattern
    float width;       // Coin line width (pixels)
};

class SoVectorizeText: public SoVectorizeItem
{
public:
    SoVectorizeText()
    {
        this->type = TEXT;
        this->fontsize = 10;
        this->col = 0;
        this->justification = LEFT;
    }

    enum Justification
    {
        LEFT,
        RIGHT,
        CENTER
    };

    SbName fontname;
    float fontsize;  // size in normalized coordinates
    SbString string;
    SbVec2f pos;  // pos in normalized coordinates
    uint32_t col;
    Justification justification;
};

class SoVectorizeImage: public SoVectorizeItem
{
public:
    SoVectorizeImage()
    {
        this->type = IMAGE;
        this->image.data = nullptr;
        this->image.nc = 0;
    }

    SbVec2f pos;   // pos in normalized coordinates
    SbVec2f size;  // size in normalized coordinates

    struct Image
    {
        const unsigned char* data;
        SbVec2s size;
        int nc;
    } image;
};

namespace Gui
{

/// Maps a number of BSPtree indices to viewport coordinates and colors.
inline void getVectorizeCoords(
    const SbVec2f& mul,
    const SbVec2f& add,
    const SbBSPTree& bsp,
    const int* vidx,
    const uint32_t* col,
    int n,
    SbVec3f* v,
    SbColor* c
)
{
    float t;
    for (int i = 0; i < n; i++) {
        v[i] = bsp.getPoint(vidx[i]);
        v[i][0] = (v[i][0] * mul[0]) + add[0];
        v[i][1] = ((1.0f - v[i][1]) * mul[1]) + add[1];
        c[i].setPackedValue(col[i], t);
    }
}

/// Dispatches a vectorize item to the matching printer of the action's P class.
template<class P>
inline void printVectorizeItem(const P* p, const SoVectorizeItem* item)
{
    switch (item->type) {
        case SoVectorizeItem::TRIANGLE:
            p->printTriangle(static_cast<const SoVectorizeTriangle*>(item));
            break;
        case SoVectorizeItem::LINE:
            p->printLine(static_cast<const SoVectorizeLine*>(item));
            break;
        case SoVectorizeItem::POINT:
            p->printPoint(static_cast<const SoVectorizePoint*>(item));
            break;
        case SoVectorizeItem::TEXT:
            p->printText(static_cast<const SoVectorizeText*>(item));
            break;
        case SoVectorizeItem::IMAGE:
            p->printImage(static_cast<const SoVectorizeImage*>(item));
            break;
        default:
            assert(0 && "unsupported item");
            break;
    }
}

}  // namespace Gui
