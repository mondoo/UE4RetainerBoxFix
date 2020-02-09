#include "SUIRetainerBoxWidget.h"
#include "Misc/App.h"
#include "UObject/Package.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/World.h"
#include "Layout/WidgetCaching.h"

DECLARE_CYCLE_STAT(TEXT("Retainer Widget Tick"), STAT_SlateRetainerWidgetTick, STATGROUP_Slate);
DECLARE_CYCLE_STAT(TEXT("Retainer Widget Paint"), STAT_SlateRetainerWidgetPaint, STATGROUP_Slate);

#if !UE_BUILD_SHIPPING
FOnUIRetainedModeChanged SUIRetainerBoxWidget::OnRetainerModeChangedDelegate;
#endif

/** True if we should allow widgets to be cached in the UI at all. */
int32 GEnableUIRetainedRendering = 1;
FAutoConsoleVariableRef EnableUIRetainedRendering(
	TEXT("Slate.EnableUIRetainedRendering"),
	GEnableUIRetainedRendering,
	TEXT("Whether to attempt to render things in SUIRetainerBoxWidgets to render targets first.")
);

static bool IsRetainedRenderingEnabled()
{
	return GEnableUIRetainedRendering != 0;
}

/** Whether or not the platform should have deferred retainer widget render target updating enabled by default */
#define PLATFORM_REQUIRES_DEFERRED_RETAINER_UPDATE PLATFORM_IOS || PLATFORM_ANDROID;

/**
 * If this is true the retained rendering render thread work will happen during normal slate render thread rendering after the back buffer has been presented
 * in order to avoid extra render target switching in the middle of the frame. The downside is that the UI update will be a frame late
 */
int32 GDeferUIRetainedRenderingRenderThread = PLATFORM_REQUIRES_DEFERRED_RETAINER_UPDATE;
FAutoConsoleVariableRef DeferUIRetainedRenderingRT(
	TEXT("Slate.DeferUIRetainedRenderingRenderThread"),
	GDeferUIRetainedRenderingRenderThread,
	TEXT("Whether or not to defer retained rendering to happen at the same time as the rest of slate render thread work"));

class FUIRetainerBoxWidgetRenderingResources : public FDeferredCleanupInterface, public FGCObject
{
public:
	FUIRetainerBoxWidgetRenderingResources()
		: WidgetRenderer(nullptr)
		, RenderTarget(nullptr)
		, DynamicEffect(nullptr)
	{}

	~FUIRetainerBoxWidgetRenderingResources()
	{
		// Note not using deferred cleanup for widget renderer here as it is already in deferred cleanup
		if (WidgetRenderer)
		{
			delete WidgetRenderer;
		}
	}

	/** FGCObject interface */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObject(RenderTarget);
		Collector.AddReferencedObject(DynamicEffect);
	}
public:
	FWidgetRenderer* WidgetRenderer;
	UTextureRenderTarget2D* RenderTarget;
	UMaterialInstanceDynamic* DynamicEffect;
};

TArray<SUIRetainerBoxWidget*, TInlineAllocator<3>> SUIRetainerBoxWidget::Shared_WaitingToRender;
int32 SUIRetainerBoxWidget::Shared_MaxRetainerWorkPerFrame(0);
TFrameValue<int32> SUIRetainerBoxWidget::Shared_RetainerWorkThisFrame(0);


SUIRetainerBoxWidget::SUIRetainerBoxWidget()
	: EmptyChildSlot(this)
	, RenderingResources(new FUIRetainerBoxWidgetRenderingResources)
{
	SetCanTick(false);
}

SUIRetainerBoxWidget::~SUIRetainerBoxWidget()
{
	if (FSlateApplication::IsInitialized())
	{
#if !UE_BUILD_SHIPPING
		OnRetainerModeChangedDelegate.RemoveAll(this);
#endif
	}

	// Begin deferred cleanup of rendering resources.  DO NOT delete here.  Will be deleted when safe
	BeginCleanup(RenderingResources);

	Shared_WaitingToRender.Remove(this);
}

void SUIRetainerBoxWidget::UpdateWidgetRenderer()
{
	const bool bWriteContentInGammaSpace = ColourSpace != EUIRetainerBoxColourSpace::Linear;

	if (!RenderingResources->WidgetRenderer)
	{
		RenderingResources->WidgetRenderer = new FWidgetRenderer(bWriteContentInGammaSpace);
	}

	UTextureRenderTarget2D* RenderTarget = RenderingResources->RenderTarget;
	FWidgetRenderer* WidgetRenderer = RenderingResources->WidgetRenderer;

	WidgetRenderer->SetUseGammaCorrection(false);
	WidgetRenderer->SetUseGammaCorrection(bWriteContentInGammaSpace ? true : false);
	WidgetRenderer->SetIsPrepassNeeded(false);
	WidgetRenderer->SetClearHitTestGrid(false);

	// Update the render target to match the current gamma rendering preferences.
	if (RenderTarget && RenderTarget->SRGB == bWriteContentInGammaSpace)
	{
		RenderTarget->TargetGamma = bWriteContentInGammaSpace ? 0.f : 1.f;
		RenderTarget->SRGB = bWriteContentInGammaSpace ? true : false;

		RenderTarget->UpdateResource();
	}
}

void SUIRetainerBoxWidget::Construct(const FArguments& InArgs)
{
	FSlateApplicationBase::Get().OnGlobalInvalidate().AddSP(this, &SUIRetainerBoxWidget::OnGlobalInvalidate);

	STAT(MyStatId = FDynamicStats::CreateStatId<FStatGroup_STATGROUP_Slate>(InArgs._StatId);)

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
	RenderTarget->ClearColor = FLinearColor::Transparent;
	RenderTarget->OverrideFormat = PF_B8G8R8A8;
	RenderTarget->bForceLinearGamma = false;

	RenderingResources->RenderTarget = RenderTarget;
	SurfaceBrush.SetResourceObject(RenderTarget);

	Window = SNew(SVirtualWindow)
		.Visibility(EVisibility::SelfHitTestInvisible);  // deubanks: We don't want Retainer Widgets blocking hit testing for tooltips

	Window->SetShouldResolveDeferred(false);

	UpdateWidgetRenderer();

	MyWidget = InArgs._Content.Widget;

	RenderOnPhase = InArgs._RenderOnPhase;
	RenderOnInvalidation = InArgs._RenderOnInvalidation;

	Phase = InArgs._Phase;
	PhaseCount = InArgs._PhaseCount;

	LastDrawTime = FApp::GetCurrentTime();
	LastTickedFrame = 0;

	bEnableUIRetainedRenderingDesire = true;
	bEnableUIRetainedRendering = false;

	bRenderRequested = true;

	RootCacheNode = nullptr;
	LastUsedCachedNodeIndex = 0;

	Window->SetContent(MyWidget.ToSharedRef());

	ChildSlot
		[
			Window.ToSharedRef()
		];

	if (FSlateApplication::IsInitialized())
	{
#if !UE_BUILD_SHIPPING
		OnRetainerModeChangedDelegate.AddRaw(this, &SUIRetainerBoxWidget::OnRetainerModeChanged);

		static bool bStaticInit = false;

		if (!bStaticInit)
		{
			bStaticInit = true;
			EnableUIRetainedRendering->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&SUIRetainerBoxWidget::OnRetainerModeCVarChanged));
		}
#endif
	}
}

bool SUIRetainerBoxWidget::ShouldBeRenderingOffscreen() const
{
	return bEnableUIRetainedRenderingDesire && IsRetainedRenderingEnabled();
}

bool SUIRetainerBoxWidget::IsAnythingVisibleToRender() const
{
	return MyWidget.IsValid() && MyWidget->GetVisibility().IsVisible();
}

void SUIRetainerBoxWidget::OnRetainerModeChanged()
{
	RefreshRenderingMode();
	Invalidate(EInvalidateWidget::Layout);
}

void SUIRetainerBoxWidget::OnGlobalInvalidate()
{
	RequestRender();
}

#if !UE_BUILD_SHIPPING
void SUIRetainerBoxWidget::OnRetainerModeCVarChanged(IConsoleVariable* CVar)
{
	OnRetainerModeChangedDelegate.Broadcast();
}
#endif

void SUIRetainerBoxWidget::SetRetainedRendering(bool bRetainRendering)
{
	bEnableUIRetainedRenderingDesire = bRetainRendering;
}

void SUIRetainerBoxWidget::RefreshRenderingMode()
{
	const bool bShouldBeRenderingOffscreen = ShouldBeRenderingOffscreen();

	if (bEnableUIRetainedRendering != bShouldBeRenderingOffscreen)
	{
		bEnableUIRetainedRendering = bShouldBeRenderingOffscreen;

		Window->SetContent(MyWidget.ToSharedRef());
	}
}

void SUIRetainerBoxWidget::SetContent(const TSharedRef< SWidget >& InContent)
{
	MyWidget = InContent;
	Window->SetContent(InContent);
}

UMaterialInstanceDynamic* SUIRetainerBoxWidget::GetEffectMaterial() const
{
	return RenderingResources->DynamicEffect;
}

void SUIRetainerBoxWidget::SetEffectMaterial(UMaterialInterface* EffectMaterial)
{
	if (EffectMaterial)
	{
		UMaterialInstanceDynamic* DynamicEffect = Cast<UMaterialInstanceDynamic>(EffectMaterial);
		if (!DynamicEffect)
		{
			DynamicEffect = UMaterialInstanceDynamic::Create(EffectMaterial, GetTransientPackage());
		}
		RenderingResources->DynamicEffect = DynamicEffect;

		SurfaceBrush.SetResourceObject(RenderingResources->DynamicEffect);
	}
	else
	{
		RenderingResources->DynamicEffect = nullptr;
		SurfaceBrush.SetResourceObject(RenderingResources->RenderTarget);
	}

	UpdateWidgetRenderer();
}

void SUIRetainerBoxWidget::SetTextureParameter(FName TextureParameter)
{
	DynamicEffectTextureParameter = TextureParameter;
}

void SUIRetainerBoxWidget::SetWorld(UWorld* World)
{
	OuterWorld = World;
}

void SUIRetainerBoxWidget::SetColourSpace(EUIRetainerBoxColourSpace InColourSpace)
{
	ColourSpace = InColourSpace;
}

FChildren* SUIRetainerBoxWidget::GetChildren()
{
	if (bEnableUIRetainedRendering)
	{
		return &EmptyChildSlot;
	}
	else
	{
		return SCompoundWidget::GetChildren();
	}
}

bool SUIRetainerBoxWidget::ComputeVolatility() const
{
	return true;
}

FCachedWidgetNode* SUIRetainerBoxWidget::CreateCacheNode() const
{
	// If the node pool is empty, allocate a few
	if (LastUsedCachedNodeIndex >= NodePool.Num())
	{
		for (int32 i = 0; i < 10; i++)
		{
			NodePool.Add(new FCachedWidgetNode());
		}
	}

	// Return one of the preallocated nodes and increment the next node index.
	FCachedWidgetNode* NewNode = NodePool[LastUsedCachedNodeIndex];
	++LastUsedCachedNodeIndex;

	return NewNode;
}

void SUIRetainerBoxWidget::InvalidateWidget(SWidget* InvalidateWidget)
{
	if (RenderOnInvalidation)
	{
		bRenderRequested = true;
	}
}

void SUIRetainerBoxWidget::SetRenderingPhase(int32 InPhase, int32 InPhaseCount)
{
	Phase = InPhase;
	PhaseCount = InPhaseCount;
}

void SUIRetainerBoxWidget::RequestRender()
{
	bRenderRequested = true;
}

bool SUIRetainerBoxWidget::PaintRetainedContent(const FPaintArgs& Args, const FGeometry& AllottedGeometry)
{
	if (RenderOnPhase)
	{
		if (LastTickedFrame != GFrameCounter && (GFrameCounter % PhaseCount) == Phase)
		{
			bRenderRequested = true;
		}
	}

	if (Shared_MaxRetainerWorkPerFrame > 0)
	{
		if (Shared_RetainerWorkThisFrame.TryGetValue(0) > Shared_MaxRetainerWorkPerFrame)
		{
			Shared_WaitingToRender.AddUnique(this);
			return false;
		}
	}

	const FPaintGeometry PaintGeometry = AllottedGeometry.ToPaintGeometry();
	const FVector2D RenderSize = PaintGeometry.GetLocalSize() * PaintGeometry.GetAccumulatedRenderTransform().GetMatrix().GetScale().GetVector();

	if (RenderSize != PreviousRenderSize)
	{
		PreviousRenderSize = RenderSize;
		bRenderRequested = true;
	}

	if (bRenderRequested)
	{
		// In order to get material parameter collections to function properly, we need the current world's Scene
		// properly propagated through to any widgets that depend on that functionality. The SceneViewport and RetainerWidget the 
		// only location where this information exists in Slate, so we push the current scene onto the current
		// Slate application so that we can leverage it in later calls.
		UWorld* TickWorld = OuterWorld.Get();
		if (TickWorld && TickWorld->Scene && IsInGameThread())
		{
			FSlateApplication::Get().GetRenderer()->RegisterCurrentScene(TickWorld->Scene);
		}
		else if (IsInGameThread())
		{
			FSlateApplication::Get().GetRenderer()->RegisterCurrentScene(nullptr);
		}

		// Update the number of retainers we've drawn this frame.
		Shared_RetainerWorkThisFrame = Shared_RetainerWorkThisFrame.TryGetValue(0) + 1;

		LastTickedFrame = GFrameCounter;
		const double TimeSinceLastDraw = FApp::GetCurrentTime() - LastDrawTime;

		const uint32 RenderTargetWidth = FMath::RoundToInt(RenderSize.X);
		const uint32 RenderTargetHeight = FMath::RoundToInt(RenderSize.Y);

		const FVector2D ViewOffset = PaintGeometry.DrawPosition.RoundToVector();

		// Keep the visibilities the same, the proxy window should maintain the same visible/non-visible hit-testing of the retainer.
		Window->SetVisibility(GetVisibility());

		// Need to prepass.
		Window->SlatePrepass(AllottedGeometry.Scale);

		// Reset the cached node pool index so that we effectively reset the pool.
		LastUsedCachedNodeIndex = 0;
		RootCacheNode = nullptr;

		UTextureRenderTarget2D* RenderTarget = RenderingResources->RenderTarget;
		FWidgetRenderer* WidgetRenderer = RenderingResources->WidgetRenderer;

		if (RenderTargetWidth != 0 && RenderTargetHeight != 0)
		{
			if (MyWidget->GetVisibility().IsVisible())
			{
				if (RenderTarget->GetSurfaceWidth() != RenderTargetWidth ||
					RenderTarget->GetSurfaceHeight() != RenderTargetHeight)
				{

					// If the render target resource already exists just resize it.  Calling InitCustomFormat flushes render commands which could result in a huge hitch
					if (RenderTarget->GameThread_GetRenderTargetResource() && RenderTarget->OverrideFormat == PF_B8G8R8A8)
					{
						RenderTarget->ResizeTarget(RenderTargetWidth, RenderTargetHeight);
					}
					else
					{
						const bool bForceLinearGamma = false;
						RenderTarget->InitCustomFormat(RenderTargetWidth, RenderTargetHeight, PF_B8G8R8A8, bForceLinearGamma);
						RenderTarget->UpdateResourceImmediate();
					}
				}

				const float Scale = AllottedGeometry.Scale;

				const FVector2D DrawSize = FVector2D(RenderTargetWidth, RenderTargetHeight);
				const FGeometry WindowGeometry = FGeometry::MakeRoot(DrawSize * (1 / Scale), FSlateLayoutTransform(Scale, PaintGeometry.DrawPosition));

				// Update the surface brush to match the latest size.
				SurfaceBrush.ImageSize = DrawSize;

				WidgetRenderer->ViewOffset = -ViewOffset;

				SUIRetainerBoxWidget* MutableThis = const_cast<SUIRetainerBoxWidget*>(this);
				TSharedRef<SUIRetainerBoxWidget> SharedMutableThis = SharedThis(MutableThis);

				FPaintArgs PaintArgs(*this, Args.GetGrid(), Args.GetWindowToDesktopTransform(), FApp::GetCurrentTime(), Args.GetDeltaTime());

				RootCacheNode = CreateCacheNode();
				RootCacheNode->Initialize(Args, SharedMutableThis, WindowGeometry);

				WidgetRenderer->DrawWindow(
					PaintArgs.EnableCaching(SharedMutableThis, RootCacheNode, true, true),
					RenderTarget,
					Window.ToSharedRef(),
					WindowGeometry,
					WindowGeometry.GetLayoutBoundingRect(),
					TimeSinceLastDraw,
					GDeferUIRetainedRenderingRenderThread != 0);

				bRenderRequested = false;
				Shared_WaitingToRender.Remove(this);

				LastDrawTime = FApp::GetCurrentTime();

				return true;
			}
		}
	}

	return false;
}

int32 SUIRetainerBoxWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	STAT(FScopeCycleCounter PaintCycleCounter(MyStatId);)

		SUIRetainerBoxWidget* MutableThis = const_cast<SUIRetainerBoxWidget*>(this);

	MutableThis->RefreshRenderingMode();

	if (bEnableUIRetainedRendering && IsAnythingVisibleToRender())
	{
		SCOPE_CYCLE_COUNTER(STAT_SlateRetainerWidgetPaint);

		TSharedRef<SUIRetainerBoxWidget> SharedMutableThis = SharedThis(MutableThis);

		const bool bNewFramePainted = MutableThis->PaintRetainedContent(Args, AllottedGeometry);

		UTextureRenderTarget2D* RenderTarget = RenderingResources->RenderTarget;

		if (RenderTarget->GetSurfaceWidth() >= 1 && RenderTarget->GetSurfaceHeight() >= 1)
		{
			const FLinearColor ComputedColorAndOpacity(InWidgetStyle.GetColorAndOpacityTint() * ColorAndOpacity.Get() * SurfaceBrush.GetTint(InWidgetStyle));
			const FLinearColor AdjustedColor(ComputedColorAndOpacity / ComputedColorAndOpacity.A);
			const FLinearColor PremultipliedColorAndOpacity(ComputedColorAndOpacity * ComputedColorAndOpacity.A);

			FWidgetRenderer* WidgetRenderer = RenderingResources->WidgetRenderer;
			UMaterialInstanceDynamic* DynamicEffect = RenderingResources->DynamicEffect;

			const bool bDynamicMaterialInUse = (DynamicEffect != nullptr);
			if (bDynamicMaterialInUse)
			{
				DynamicEffect->SetTextureParameterValue(DynamicEffectTextureParameter, RenderTarget);
			}

			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(),
				&SurfaceBrush,
				// We always write out the content in gamma space, so when we render the final version we need to
				// render without gamma correction enabled.
				ColourSpace != EUIRetainerBoxColourSpace::Linear
					? ESlateDrawEffect::PreMultipliedAlpha | ESlateDrawEffect::NoGamma
					: ESlateDrawEffect::None,
				ColourSpace != EUIRetainerBoxColourSpace::Linear 
					? FLinearColor(AdjustedColor.R, AdjustedColor.G, AdjustedColor.B, ComputedColorAndOpacity.A)
					: FLinearColor(PremultipliedColorAndOpacity.R, PremultipliedColorAndOpacity.G, PremultipliedColorAndOpacity.B, PremultipliedColorAndOpacity.A)
			);

			if (RootCacheNode)
			{
				RootCacheNode->RecordHittestGeometry(Args.GetGrid(), Args.GetLastHitTestIndex(), LayerId, FVector2D(0, 0));
			}

			// Any deferred painted elements of the retainer should be drawn directly by the main renderer, not rendered into the render target,
			// as most of those sorts of things will break the rendering rect, things like tooltips, and popup menus.
			for (auto& DeferredPaint : WidgetRenderer->DeferredPaints)
			{
				OutDrawElements.QueueDeferredPainting(DeferredPaint->Copy(Args));
			}
		}

		return LayerId;
	}

	return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
}

FVector2D SUIRetainerBoxWidget::ComputeDesiredSize(float LayoutScaleMuliplier) const
{
	if (bEnableUIRetainedRendering)
	{
		return MyWidget->GetDesiredSize();
	}
	else
	{
		return SCompoundWidget::ComputeDesiredSize(LayoutScaleMuliplier);
	}
}