/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 sts=2 ts=8 et tw=99 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_LAYERS_WEBRENDERAPI_H
#define MOZILLA_LAYERS_WEBRENDERAPI_H

#include <vector>
#include <unordered_map>

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/layers/SyncObject.h"
#include "mozilla/Range.h"
#include "mozilla/webrender/webrender_ffi.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "FrameMetrics.h"
#include "GLTypes.h"
#include "Units.h"

namespace mozilla {

namespace widget {
class CompositorWidget;
}

namespace layers {
class CompositorBridgeParentBase;
class WebRenderBridgeParent;
}

namespace wr {

class DisplayListBuilder;
class RendererOGL;
class RendererEvent;

// This isn't part of WR's API, but we define it here to simplify layout's
// logic and data plumbing.
struct Line {
  float baseline;
  float start;
  float end;
  float width;
  wr::ColorF color;
  wr::LineOrientation orientation;
  wr::LineStyle style;
};

/// Updates to retained resources such as images and fonts, applied within the
/// same transaction.
class ResourceUpdateQueue {
public:
  ResourceUpdateQueue();
  ~ResourceUpdateQueue();
  ResourceUpdateQueue(ResourceUpdateQueue&&);
  ResourceUpdateQueue(const ResourceUpdateQueue&) = delete;
  ResourceUpdateQueue& operator=(ResourceUpdateQueue&&);
  ResourceUpdateQueue& operator=(const ResourceUpdateQueue&) = delete;

  void AddImage(wr::ImageKey aKey,
                const ImageDescriptor& aDescriptor,
                wr::Vec_u8& aBytes);

  void AddBlobImage(wr::ImageKey aKey,
                    const ImageDescriptor& aDescriptor,
                    wr::Vec_u8& aBytes);

  void AddExternalImageBuffer(ImageKey key,
                              const ImageDescriptor& aDescriptor,
                              ExternalImageId aHandle);

  void AddExternalImage(ImageKey key,
                        const ImageDescriptor& aDescriptor,
                        ExternalImageId aExtID,
                        WrExternalImageBufferType aBufferType,
                        uint8_t aChannelIndex = 0);

  void UpdateImageBuffer(wr::ImageKey aKey,
                         const ImageDescriptor& aDescriptor,
                         wr::Vec_u8& aBytes);

  void UpdateBlobImage(wr::ImageKey aKey,
                       const ImageDescriptor& aDescriptor,
                       wr::Vec_u8& aBytes);

  void UpdateExternalImage(ImageKey aKey,
                           const ImageDescriptor& aDescriptor,
                           ExternalImageId aExtID,
                           wr::WrExternalImageBufferType aBufferType,
                           uint8_t aChannelIndex = 0);

  void DeleteImage(wr::ImageKey aKey);

  void AddRawFont(wr::FontKey aKey, wr::Vec_u8& aBytes, uint32_t aIndex);

  void DeleteFont(wr::FontKey aKey);

  void AddFontInstance(wr::FontInstanceKey aKey,
                       wr::FontKey aFontKey,
                       float aGlyphSize,
                       const wr::FontInstanceOptions* aOptions,
                       const wr::FontInstancePlatformOptions* aPlatformOptions,
                       wr::Vec_u8& aVariations);

  void DeleteFontInstance(wr::FontInstanceKey aKey);

  void Clear();

  // Try to avoid using this when possible.
  wr::ResourceUpdates* Raw() { return mUpdates; }

protected:
  explicit ResourceUpdateQueue(wr::ResourceUpdates* aUpdates)
  : mUpdates(aUpdates) {}

  wr::ResourceUpdates* mUpdates;
};

class WebRenderAPI
{
  NS_INLINE_DECL_REFCOUNTING(WebRenderAPI);

public:
  /// This can be called on the compositor thread only.
  static already_AddRefed<WebRenderAPI> Create(layers::CompositorBridgeParentBase* aBridge,
                                               RefPtr<widget::CompositorWidget>&& aWidget,
                                               LayoutDeviceIntSize aSize);

  already_AddRefed<WebRenderAPI> Clone();

  wr::WindowId GetId() const { return mId; }

  void UpdateScrollPosition(const wr::WrPipelineId& aPipelineId,
                            const layers::FrameMetrics::ViewID& aScrollId,
                            const wr::LayoutPoint& aScrollPosition);

  void GenerateFrame();
  void GenerateFrame(const nsTArray<wr::WrOpacityProperty>& aOpacityArray,
                     const nsTArray<wr::WrTransformProperty>& aTransformArray);

  void SetWindowParameters(LayoutDeviceIntSize size);

  void SetDisplayList(gfx::Color aBgColor,
                      Epoch aEpoch,
                      mozilla::LayerSize aViewportSize,
                      wr::WrPipelineId pipeline_id,
                      const wr::LayoutSize& content_size,
                      wr::BuiltDisplayListDescriptor dl_descriptor,
                      uint8_t *dl_data,
                      size_t dl_size,
                      ResourceUpdateQueue& aResources);

  void ClearDisplayList(Epoch aEpoch, wr::WrPipelineId pipeline_id);

  void SetRootPipeline(wr::PipelineId aPipeline);

  void UpdateResources(ResourceUpdateQueue& aUpdates);

  void SetFrameStartTime(const TimeStamp& aTime);

  void RunOnRenderThread(UniquePtr<RendererEvent> aEvent);

  void Readback(gfx::IntSize aSize, uint8_t *aBuffer, uint32_t aBufferSize);

  void Pause();
  bool Resume();

  wr::WrIdNamespace GetNamespace();
  uint32_t GetMaxTextureSize() const { return mMaxTextureSize; }
  bool GetUseANGLE() const { return mUseANGLE; }
  layers::SyncHandle GetSyncHandle() const { return mSyncHandle; }

protected:
  WebRenderAPI(wr::DocumentHandle* aHandle, wr::WindowId aId, uint32_t aMaxTextureSize, bool aUseANGLE, layers::SyncHandle aSyncHandle)
    : mDocHandle(aHandle)
    , mId(aId)
    , mMaxTextureSize(aMaxTextureSize)
    , mUseANGLE(aUseANGLE)
    , mSyncHandle(aSyncHandle)
  {}

  ~WebRenderAPI();
  // Should be used only for shutdown handling
  void WaitFlushed();

  wr::DocumentHandle* mDocHandle;
  wr::WindowId mId;
  uint32_t mMaxTextureSize;
  bool mUseANGLE;
  layers::SyncHandle mSyncHandle;
  RefPtr<wr::WebRenderAPI> mRootApi;

  friend class DisplayListBuilder;
  friend class layers::WebRenderBridgeParent;
};

/// This is a simple C++ wrapper around WrState defined in the rust bindings.
/// We may want to turn this into a direct wrapper on top of WebRenderFrameBuilder
/// instead, so the interface may change a bit.
class DisplayListBuilder {
public:
  explicit DisplayListBuilder(wr::PipelineId aId,
                              const wr::LayoutSize& aContentSize);
  DisplayListBuilder(DisplayListBuilder&&) = default;

  ~DisplayListBuilder();

  void Finalize(wr::LayoutSize& aOutContentSize,
                wr::BuiltDisplayList& aOutDisplayList);

  void PushStackingContext(const wr::LayoutRect& aBounds, // TODO: We should work with strongly typed rects
                           const uint64_t& aAnimationId,
                           const float* aOpacity,
                           const gfx::Matrix4x4* aTransform,
                           wr::TransformStyle aTransformStyle,
                           const gfx::Matrix4x4* aPerspective,
                           const wr::MixBlendMode& aMixBlendMode,
                           const nsTArray<wr::WrFilterOp>& aFilters,
                           bool aIsBackfaceVisible);
  void PopStackingContext();

  wr::WrClipId DefineClip(const wr::LayoutRect& aClipRect,
                          const nsTArray<wr::WrComplexClipRegion>* aComplex = nullptr,
                          const wr::WrImageMask* aMask = nullptr);
  void PushClip(const wr::WrClipId& aClipId, bool aMask = false);
  void PopClip(bool aMask = false);

  wr::WrStickyId DefineStickyFrame(const wr::LayoutRect& aContentRect,
                                   const wr::StickySideConstraint* aTop,
                                   const wr::StickySideConstraint* aRight,
                                   const wr::StickySideConstraint* aBottom,
                                   const wr::StickySideConstraint* aLeft);
  void PushStickyFrame(const wr::WrStickyId& aStickyId);
  void PopStickyFrame();

  void PushBuiltDisplayList(wr::BuiltDisplayList &dl);

  bool IsScrollLayerDefined(layers::FrameMetrics::ViewID aScrollId) const;
  void DefineScrollLayer(const layers::FrameMetrics::ViewID& aScrollId,
                         const wr::LayoutRect& aContentRect, // TODO: We should work with strongly typed rects
                         const wr::LayoutRect& aClipRect);
  void PushScrollLayer(const layers::FrameMetrics::ViewID& aScrollId);
  void PopScrollLayer();

  void PushClipAndScrollInfo(const layers::FrameMetrics::ViewID& aScrollId,
                             const wr::WrClipId* aClipId);
  void PopClipAndScrollInfo();

  void PushRect(const wr::LayoutRect& aBounds,
                const wr::LayoutRect& aClip,
                bool aIsBackfaceVisible,
                const wr::ColorF& aColor);

  void PushLinearGradient(const wr::LayoutRect& aBounds,
                          const wr::LayoutRect& aClip,
                          bool aIsBackfaceVisible,
                          const wr::LayoutPoint& aStartPoint,
                          const wr::LayoutPoint& aEndPoint,
                          const nsTArray<wr::GradientStop>& aStops,
                          wr::ExtendMode aExtendMode,
                          const wr::LayoutSize aTileSize,
                          const wr::LayoutSize aTileSpacing);

  void PushRadialGradient(const wr::LayoutRect& aBounds,
                          const wr::LayoutRect& aClip,
                          bool aIsBackfaceVisible,
                          const wr::LayoutPoint& aCenter,
                          const wr::LayoutSize& aRadius,
                          const nsTArray<wr::GradientStop>& aStops,
                          wr::ExtendMode aExtendMode,
                          const wr::LayoutSize aTileSize,
                          const wr::LayoutSize aTileSpacing);

  void PushImage(const wr::LayoutRect& aBounds,
                 const wr::LayoutRect& aClip,
                 bool aIsBackfaceVisible,
                 wr::ImageRendering aFilter,
                 wr::ImageKey aImage);

  void PushImage(const wr::LayoutRect& aBounds,
                 const wr::LayoutRect& aClip,
                 bool aIsBackfaceVisible,
                 const wr::LayoutSize& aStretchSize,
                 const wr::LayoutSize& aTileSpacing,
                 wr::ImageRendering aFilter,
                 wr::ImageKey aImage);

  void PushYCbCrPlanarImage(const wr::LayoutRect& aBounds,
                            const wr::LayoutRect& aClip,
                            bool aIsBackfaceVisible,
                            wr::ImageKey aImageChannel0,
                            wr::ImageKey aImageChannel1,
                            wr::ImageKey aImageChannel2,
                            wr::WrYuvColorSpace aColorSpace,
                            wr::ImageRendering aFilter);

  void PushNV12Image(const wr::LayoutRect& aBounds,
                     const wr::LayoutRect& aClip,
                     bool aIsBackfaceVisible,
                     wr::ImageKey aImageChannel0,
                     wr::ImageKey aImageChannel1,
                     wr::WrYuvColorSpace aColorSpace,
                     wr::ImageRendering aFilter);

  void PushYCbCrInterleavedImage(const wr::LayoutRect& aBounds,
                                 const wr::LayoutRect& aClip,
                                 bool aIsBackfaceVisible,
                                 wr::ImageKey aImageChannel0,
                                 wr::WrYuvColorSpace aColorSpace,
                                 wr::ImageRendering aFilter);

  void PushIFrame(const wr::LayoutRect& aBounds,
                  bool aIsBackfaceVisible,
                  wr::PipelineId aPipeline);

  // XXX WrBorderSides are passed with Range.
  // It is just to bypass compiler bug. See Bug 1357734.
  void PushBorder(const wr::LayoutRect& aBounds,
                  const wr::LayoutRect& aClip,
                  bool aIsBackfaceVisible,
                  const wr::BorderWidths& aWidths,
                  const Range<const wr::BorderSide>& aSides,
                  const wr::BorderRadius& aRadius);

  void PushBorderImage(const wr::LayoutRect& aBounds,
                       const wr::LayoutRect& aClip,
                       bool aIsBackfaceVisible,
                       const wr::BorderWidths& aWidths,
                       wr::ImageKey aImage,
                       const wr::NinePatchDescriptor& aPatch,
                       const wr::SideOffsets2D_f32& aOutset,
                       const wr::RepeatMode& aRepeatHorizontal,
                       const wr::RepeatMode& aRepeatVertical);

  void PushBorderGradient(const wr::LayoutRect& aBounds,
                          const wr::LayoutRect& aClip,
                          bool aIsBackfaceVisible,
                          const wr::BorderWidths& aWidths,
                          const wr::LayoutPoint& aStartPoint,
                          const wr::LayoutPoint& aEndPoint,
                          const nsTArray<wr::GradientStop>& aStops,
                          wr::ExtendMode aExtendMode,
                          const wr::SideOffsets2D_f32& aOutset);

  void PushBorderRadialGradient(const wr::LayoutRect& aBounds,
                                const wr::LayoutRect& aClip,
                                bool aIsBackfaceVisible,
                                const wr::BorderWidths& aWidths,
                                const wr::LayoutPoint& aCenter,
                                const wr::LayoutSize& aRadius,
                                const nsTArray<wr::GradientStop>& aStops,
                                wr::ExtendMode aExtendMode,
                                const wr::SideOffsets2D_f32& aOutset);

  void PushText(const wr::LayoutRect& aBounds,
                const wr::LayoutRect& aClip,
                bool aIsBackfaceVisible,
                const gfx::Color& aColor,
                wr::FontInstanceKey aFontKey,
                Range<const wr::GlyphInstance> aGlyphBuffer,
                const wr::GlyphOptions* aGlyphOptions = nullptr);

  void PushLine(const wr::LayoutRect& aClip,
                bool aIsBackfaceVisible,
                const wr::Line& aLine);

  void PushTextShadow(const wr::LayoutRect& aBounds,
                      const wr::LayoutRect& aClip,
                      bool aIsBackfaceVisible,
                      const wr::TextShadow& aShadow);

  void PopTextShadow();



  void PushBoxShadow(const wr::LayoutRect& aRect,
                     const wr::LayoutRect& aClip,
                     bool aIsBackfaceVisible,
                     const wr::LayoutRect& aBoxBounds,
                     const wr::LayoutVector2D& aOffset,
                     const wr::ColorF& aColor,
                     const float& aBlurRadius,
                     const float& aSpreadRadius,
                     const float& aBorderRadius,
                     const wr::BoxShadowClipMode& aClipMode);

  // Returns the clip id that was most recently pushed with PushClip and that
  // has not yet been popped with PopClip. Return Nothing() if the clip stack
  // is empty.
  Maybe<wr::WrClipId> TopmostClipId();
  // Same as TopmostClipId() but for scroll layers.
  layers::FrameMetrics::ViewID TopmostScrollId();
  // Returns the scroll id that was pushed just before the given scroll id. This
  // function returns Nothing() if the given scrollid has not been encountered,
  // or if it is the rootmost scroll id (and therefore has no ancestor).
  Maybe<layers::FrameMetrics::ViewID> ParentScrollIdFor(layers::FrameMetrics::ViewID aScrollId);

  // Try to avoid using this when possible.
  wr::WrState* Raw() { return mWrState; }

  // Return true if the current clip stack has any mask type clip.
  bool HasMaskClip() { return mMaskClipCount > 0; }

protected:
  wr::WrState* mWrState;

  // Track the stack of clip ids and scroll layer ids that have been pushed
  // (by PushClip and PushScrollLayer, respectively) and are still active.
  // This is helpful for knowing e.g. what the ancestor scroll id of a particular
  // scroll id is, and doing other "queries" of current state.
  std::vector<wr::WrClipId> mClipIdStack;
  std::vector<layers::FrameMetrics::ViewID> mScrollIdStack;

  // Track the parent scroll id of each scroll id that we encountered. A
  // Nothing() value indicates a root scroll id. We also use this structure to
  // ensure that we don't define a particular scroll layer multiple times.
  std::unordered_map<layers::FrameMetrics::ViewID, Maybe<layers::FrameMetrics::ViewID>> mScrollParents;

  // The number of mask clips that are in the stack.
  uint32_t mMaskClipCount;

  friend class WebRenderAPI;
};

Maybe<wr::ImageFormat>
SurfaceFormatToImageFormat(gfx::SurfaceFormat aFormat);

} // namespace wr
} // namespace mozilla

#endif
