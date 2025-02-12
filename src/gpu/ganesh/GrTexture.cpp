/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkMath.h"
#include "include/core/SkTypes.h"
#include "include/gpu/GrTypes.h"
#include "src/core/SkMipmap.h"
#include "src/gpu/ganesh/GrCaps.h"
#include "src/gpu/ganesh/GrGpu.h"
#include "src/gpu/ganesh/GrRenderTarget.h"
#include "src/gpu/ganesh/GrResourceCache.h"
#include "src/gpu/ganesh/GrTexture.h"

#if defined(SK_DEBUG)
#include "include/gpu/GrDirectContext.h"
#include "src/gpu/ganesh/GrDirectContextPriv.h"
#include "src/gpu/ganesh/GrDrawingManager.h"
#include "src/gpu/ganesh/GrTextureProxy.h"
#if defined(SK_GL)
#include "src/gpu/ganesh/gl/GrGLTexture.h"
#endif
#endif

void GrTexture::markMipmapsDirty(const char* reason) {
    if (GrMipmapStatus::kValid == fMipmapStatus) {
        fMipmapStatus = GrMipmapStatus::kDirty;
#if defined(SK_DEBUG)
        fMipmapDirtyReason = reason;
        if (auto* context = this->getContext()) {
            fMipmapDirtyFlushNum    = context->priv().drawingManager()->flushNumber();
            fMipmapDirtyWasFlushing = context->priv().drawingManager()->isFlushing();
        }
#endif
    }
}

void GrTexture::markMipmapsClean() {
    SkASSERT(GrMipmapStatus::kNotAllocated != fMipmapStatus);
    SkDEBUGCODE(fMipmapRegenFailureReason = "did not fail";)
    fMipmapStatus = GrMipmapStatus::kValid;
}

#if defined(SK_DEBUG)
void GrTexture::assertMipmapsNotDirty(const GrTextureProxy* proxy) {
    // There are some cases where we might be given a non-mipmapped texture with a
    // mipmap filter. See skbug.com/7094.
    if (this->mipmapped() == GrMipmapped::kYes && this->mipmapsAreDirty()) {
        SkString msg("MM dirty unexpectedly.");
        if (auto* context = this->getContext()) {
            int  flushNum   = context->priv().drawingManager()->flushNumber();
            bool isFlushing = context->priv().drawingManager()->isFlushing();

            auto flushStr = [](int num, bool is) {
                return SkStringPrintf("%s flush #%d", is ? "in" : "before", num);
            };

            bool isRT = false;
            int sampleCount = 1;
            if (auto* rt = this->asRenderTarget()) {
                isRT = true;
                sampleCount = rt->numSamples();
            }
            int format = 0;
            int borrowed = -1;
#if defined(SK_GL)
            format = (int)this->backendFormat().asGLFormat();
            if (context->backend() == GrBackendApi::kOpenGL) {
                auto gltex = static_cast<GrGLTexture*>(this);
                borrowed = SkToInt(gltex->idOwnership() == GrBackendObjectOwnership::kBorrowed);
            }
#endif
            msg += SkStringPrintf(
                    " Dirtied by \"%s\" %s, now we're %s. "
                    "tex dims: %dx%d, gl fmt: %04x, isRT: %d, sc: %d, borrowed: %d, type:%d, ro:%d,"
                    " regen failed: \"%s\"",
                    fMipmapDirtyReason,
                    flushStr(fMipmapDirtyFlushNum, fMipmapDirtyWasFlushing).c_str(),
                    flushStr(flushNum, isFlushing).c_str(),
                    this->width(),
                    this->height(),
                    format,
                    isRT,
                    sampleCount,
                    borrowed,
                    (int)this->textureType(),
                    this->readOnly(),
                    fMipmapRegenFailureReason);
        }
        if (proxy) {
            msg += SkStringPrintf(", proxy status = %d, slated: %d ",
                                  proxy->mipmapsAreDirty(),
                                  proxy->slatedForMipmapRegen());
            if (proxy->mipmapsAreDirty()) {
                msg += proxy->mipmapDirtyReport();
            }
        }
        SK_ABORT("%s", msg.c_str());
    }
}
#endif

size_t GrTexture::onGpuMemorySize() const {
    return GrSurface::ComputeSize(this->backendFormat(), this->dimensions(),
                                  /*colorSamplesPerPixel=*/1, this->mipmapped());
}

/////////////////////////////////////////////////////////////////////////////
GrTexture::GrTexture(GrGpu* gpu,
                     const SkISize& dimensions,
                     GrProtected isProtected,
                     GrTextureType textureType,
                     GrMipmapStatus mipmapStatus,
                     std::string_view label)
        : INHERITED(gpu, dimensions, isProtected, label)
        , fTextureType(textureType)
        , fMipmapStatus(mipmapStatus) {
    if (fMipmapStatus == GrMipmapStatus::kNotAllocated) {
        fMaxMipmapLevel = 0;
    } else {
        fMaxMipmapLevel = SkMipmap::ComputeLevelCount(this->width(), this->height());
    }
#if defined(SK_DEBUG)
    if (fMipmapStatus == GrMipmapStatus::kDirty) {
        fMipmapDirtyWasFlushing = gpu->getContext()->priv().drawingManager()->isFlushing();
        fMipmapDirtyFlushNum    = gpu->getContext()->priv().drawingManager()->flushNumber();
    }
#endif
    if (textureType == GrTextureType::kExternal) {
        this->setReadOnly();
    }
}

bool GrTexture::StealBackendTexture(sk_sp<GrTexture> texture,
                                    GrBackendTexture* backendTexture,
                                    SkImage::BackendTextureReleaseProc* releaseProc) {
    if (!texture->unique()) {
        return false;
    }

    if (!texture->onStealBackendTexture(backendTexture, releaseProc)) {
        return false;
    }
#ifdef SK_DEBUG
    GrResourceCache* cache = texture->getContext()->priv().getResourceCache();
    int preCount = cache->getResourceCount();
#endif
    // Ensure that the texture will be released by the cache when we drop the last ref.
    // A texture that has no refs and no keys should be immediately removed.
    if (texture->getUniqueKey().isValid()) {
        texture->resourcePriv().removeUniqueKey();
    }
    if (texture->resourcePriv().getScratchKey().isValid()) {
        texture->resourcePriv().removeScratchKey();
    }
#ifdef SK_DEBUG
    texture.reset();
    int postCount = cache->getResourceCount();
    SkASSERT(postCount < preCount);
#endif
    return true;
}

void GrTexture::computeScratchKey(skgpu::ScratchKey* key) const {
    if (!this->getGpu()->caps()->isFormatCompressed(this->backendFormat())) {
        int sampleCount = 1;
        GrRenderable renderable = GrRenderable::kNo;
        if (const auto* rt = this->asRenderTarget()) {
            sampleCount = rt->numSamples();
            renderable = GrRenderable::kYes;
        }
        auto isProtected = this->isProtected() ? GrProtected::kYes : GrProtected::kNo;
        ComputeScratchKey(*this->getGpu()->caps(), this->backendFormat(), this->dimensions(),
                          renderable, sampleCount, this->mipmapped(), isProtected, key);
    }
}

void GrTexture::ComputeScratchKey(const GrCaps& caps,
                                  const GrBackendFormat& format,
                                  SkISize dimensions,
                                  GrRenderable renderable,
                                  int sampleCnt,
                                  GrMipmapped mipmapped,
                                  GrProtected isProtected,
                                  skgpu::ScratchKey* key) {
    static const skgpu::ScratchKey::ResourceType kType = skgpu::ScratchKey::GenerateResourceType();
    SkASSERT(!dimensions.isEmpty());
    SkASSERT(sampleCnt > 0);
    SkASSERT(1 == sampleCnt || renderable == GrRenderable::kYes);

    SkASSERT(static_cast<uint32_t>(mipmapped) <= 1);
    SkASSERT(static_cast<uint32_t>(isProtected) <= 1);
    SkASSERT(static_cast<uint32_t>(renderable) <= 1);
    SkASSERT(static_cast<uint32_t>(sampleCnt) < (1 << (32 - 3)));

    uint64_t formatKey = caps.computeFormatKey(format);

    skgpu::ScratchKey::Builder builder(key, kType, 5);
    builder[0] = dimensions.width();
    builder[1] = dimensions.height();
    builder[2] = formatKey & 0xFFFFFFFF;
    builder[3] = (formatKey >> 32) & 0xFFFFFFFF;
    builder[4] = (static_cast<uint32_t>(mipmapped)   << 0)
               | (static_cast<uint32_t>(isProtected) << 1)
               | (static_cast<uint32_t>(renderable)  << 2)
               | (static_cast<uint32_t>(sampleCnt)   << 3);
}
