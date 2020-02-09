#include "UIRetainerBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

#include "SUIRetainerBoxWidget.h"

#define LOCTEXT_NAMESPACE "UMG"

static FName DefaultTextureParameterName("Texture");

/////////////////////////////////////////////////////
// URetainerBox

UUIRetainerBox::UUIRetainerBox(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Visibility = ESlateVisibility::Visible;
	Phase = 0;
	PhaseCount = 1;
	RenderOnPhase = true;
	RenderOnInvalidation = false;
	TextureParameter = DefaultTextureParameterName;
}

void UUIRetainerBox::SetRenderingPhase(int PhaseToRenderOn, int32 TotalRenderingPhases)
{
	Phase = PhaseToRenderOn;
	PhaseCount = TotalRenderingPhases;

	if (PhaseCount < 1)
	{
		PhaseCount = 1;
	}

	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->SetRenderingPhase(Phase, PhaseCount);
	}
}

void UUIRetainerBox::RequestRender()
{
	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->RequestRender();
	}
}

UMaterialInstanceDynamic* UUIRetainerBox::GetEffectMaterial() const
{
	if (MyRetainerWidget.IsValid())
	{
		return MyRetainerWidget->GetEffectMaterial();
	}

	return nullptr;
}

void UUIRetainerBox::SetEffectMaterial(UMaterialInterface* InEffectMaterial)
{
	EffectMaterial = InEffectMaterial;
	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->SetEffectMaterial(EffectMaterial);
	}
}

void UUIRetainerBox::SetSetColourSpace(EUIRetainerBoxColourSpace InColourSpace)
{
	ColourSpace = InColourSpace;
	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->SetColourSpace(ColourSpace);
	}
}

void UUIRetainerBox::SetTextureParameter(FName InTextureParameter)
{
	TextureParameter = InTextureParameter;
	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->SetTextureParameter(TextureParameter);
	}
}

void UUIRetainerBox::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	MyRetainerWidget.Reset();
}

TSharedRef<SWidget> UUIRetainerBox::RebuildWidget()
{
	MyRetainerWidget =
		SNew(SUIRetainerBoxWidget)
		.RenderOnInvalidation(RenderOnInvalidation)
		.RenderOnPhase(RenderOnPhase)
		.Phase(Phase)
		.PhaseCount(PhaseCount)
#if STATS
		.StatId(FName(*FString::Printf(TEXT("%s [%s]"), *GetFName().ToString(), *GetClass()->GetName())))
#endif//STATS
		;

	MyRetainerWidget->SetRetainedRendering(IsDesignTime() ? false : true);

	if (GetChildrenCount() > 0)
	{
		MyRetainerWidget->SetContent(GetContentSlot()->Content ? GetContentSlot()->Content->TakeWidget() : SNullWidget::NullWidget);
	}

	return MyRetainerWidget.ToSharedRef();
}

void UUIRetainerBox::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	MyRetainerWidget->SetEffectMaterial(EffectMaterial);
	MyRetainerWidget->SetTextureParameter(TextureParameter);
	MyRetainerWidget->SetWorld(GetWorld());
	MyRetainerWidget->SetColourSpace(ColourSpace);
}

void UUIRetainerBox::OnSlotAdded(UPanelSlot* InSlot)
{
	// Add the child to the live slot if it already exists
	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->SetContent(InSlot->Content ? InSlot->Content->TakeWidget() : SNullWidget::NullWidget);
	}
}

void UUIRetainerBox::OnSlotRemoved(UPanelSlot* InSlot)
{
	// Remove the widget from the live slot if it exists.
	if (MyRetainerWidget.IsValid())
	{
		MyRetainerWidget->SetContent(SNullWidget::NullWidget);
	}
}

#if WITH_EDITOR

const FText UUIRetainerBox::GetPaletteCategory()
{
	return LOCTEXT("Optimization", "Optimization");
}

#endif

const FGeometry& UUIRetainerBox::GetCachedAllottedGeometry() const
{
	if (MyRetainerWidget.IsValid())
	{
		return MyRetainerWidget->GetCachedGeometry();
	}

	static const FGeometry TempGeo;
	return TempGeo;
}

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
