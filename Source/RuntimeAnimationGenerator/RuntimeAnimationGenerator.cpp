/******************************************************************************/
/* Animation generation utilities for UE5.3                                   */
/* -------------------------------------------------------------------------- */
/* License MIT                                                                */
/* Kindly sponsored by IMVU                                                   */
/* -------------------------------------------------------------------------- */
/* Runtime helpers to prepare animation tracks and build a transient sequence.*/
/******************************************************************************/
#include "RuntimeAnimationGenerator.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AnimationUtils.h"
#include "ReferenceSkeleton.h"

DEFINE_LOG_CATEGORY_STATIC(LogRuntimeAnimationGenerator, Log, All);

namespace
{
constexpr double DefaultFrameIntervalSeconds = 1.0 / 30.0;
constexpr double FrameRatePrecision = 1000000.0;

double ResolveFrameIntervalAndDuration(
	const TArray<FRuntimeAnimationGenerator::FTrack>& Tracks,
	double& OutSequenceDuration)
{
	double FrameInterval = TNumericLimits<double>::Max();
	OutSequenceDuration = 0.0;

	for (const FRuntimeAnimationGenerator::FTrack& Track : Tracks)
	{
		if (Track.KeyFrames.IsEmpty())
		{
			continue;
		}

		double PreviousFrameTime = Track.KeyFrames[0].Time;
		OutSequenceDuration = FMath::Max(OutSequenceDuration, PreviousFrameTime);

		for (int32 KeyFrameIndex = 1; KeyFrameIndex < Track.KeyFrames.Num(); ++KeyFrameIndex)
		{
			const double CurrentTime = Track.KeyFrames[KeyFrameIndex].Time;
			const double Delta = CurrentTime - PreviousFrameTime;
			if (Delta > UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				FrameInterval = FMath::Min(FrameInterval, Delta);
			}

			OutSequenceDuration = FMath::Max(OutSequenceDuration, CurrentTime);
			PreviousFrameTime = CurrentTime;
		}
	}

	if (FrameInterval == TNumericLimits<double>::Max())
	{
		FrameInterval = DefaultFrameIntervalSeconds;
	}

	return FrameInterval;
}

FFrameRate MakeFrameRateFromInterval(const double FrameIntervalSeconds)
{
	const int64 Denominator = FMath::Clamp<int64>(
		FMath::RoundToInt64(FrameIntervalSeconds * FrameRatePrecision),
		1,
		MAX_uint32);
	return FFrameRate(static_cast<uint32>(FrameRatePrecision), static_cast<uint32>(Denominator));
}

void BuildUniformTrackKeys(
	const FRuntimeAnimationGenerator::FTrack& Track,
	const int32 NumFrames,
	const double FrameIntervalSeconds,
	TArray<FVector3f>& OutPosKeys,
	TArray<FQuat4f>& OutRotKeys,
	TArray<FVector3f>& OutScaleKeys)
{
	OutPosKeys.SetNumUninitialized(NumFrames);
	OutRotKeys.SetNumUninitialized(NumFrames);
	OutScaleKeys.SetNumUninitialized(NumFrames);

	int32 FrameId = 0;
	int32 NextFrameId = Track.KeyFrames.Num() > 1 ? 1 : 0;

	for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
	{
		const double Time = FrameIntervalSeconds * static_cast<double>(FrameIndex);

		while (NextFrameId < Track.KeyFrames.Num() && Time >= Track.KeyFrames[NextFrameId].Time)
		{
			FrameId = NextFrameId;
			NextFrameId = FMath::Min(NextFrameId + 1, Track.KeyFrames.Num() - 1);
			if (NextFrameId == FrameId)
			{
				break;
			}
		}

		const FRuntimeAnimationGenerator::FKeyFrame& Frame0 = Track.KeyFrames[FrameId];
		const FRuntimeAnimationGenerator::FKeyFrame& Frame1 = Track.KeyFrames[NextFrameId];

		if (FrameId == NextFrameId || FMath::IsNearlyEqual(Frame0.Time, Frame1.Time))
		{
			OutPosKeys[FrameIndex] = FVector3f(Frame0.Position);
			OutRotKeys[FrameIndex] = FQuat4f(Frame0.Rotation);
			OutScaleKeys[FrameIndex] = FVector3f(Frame0.Scale);
			continue;
		}

		const double Alpha = FMath::Clamp((Time - Frame0.Time) / (Frame1.Time - Frame0.Time), 0.0, 1.0);
		OutPosKeys[FrameIndex] = FVector3f(FMath::Lerp(Frame0.Position, Frame1.Position, Alpha));
		OutRotKeys[FrameIndex] = FQuat4f(FQuat::Slerp(Frame0.Rotation, Frame1.Rotation, Alpha));
		OutScaleKeys[FrameIndex] = FVector3f(FMath::Lerp(Frame0.Scale, Frame1.Scale, Alpha));
	}
}
} // namespace

void FRuntimeAnimationGeneratorModule::StartupModule()
{
}

void FRuntimeAnimationGeneratorModule::ShutdownModule()
{
}

FRuntimeAnimationGenerator::FKeyFrame::FKeyFrame(
	const double Time,
	const FVector& Position,
	const FQuat& Rotation,
	const FVector& Scale)
	: Time(Time)
	, Position(Position)
	, Rotation(Rotation)
	, Scale(Scale)
{
}

IMPLEMENT_MODULE(FRuntimeAnimationGeneratorModule, RuntimeAnimationGenerator)

void FRuntimeAnimationGenerator::PrepareSkeletonTracks(const USkeleton* Skeleton, FTracks& OutTracks)
{
	OutTracks.IsReady = false;

	if (Skeleton == nullptr)
	{
		UE_LOG(LogRuntimeAnimationGenerator, Warning, TEXT("PrepareSkeletonTracks requires a valid skeleton."));
		OutTracks.Tracks.Reset();
		return;
	}

	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();

	for (int32 TrackIndex = OutTracks.Tracks.Num() - 1; TrackIndex >= 0; --TrackIndex)
	{
		const FTrack& Track = OutTracks.Tracks[TrackIndex];
		if (Track.KeyFrames.IsEmpty() || ReferenceSkeleton.FindBoneIndex(Track.BoneName) == INDEX_NONE)
		{
			OutTracks.Tracks.RemoveAt(TrackIndex);
		}
	}

	for (FTrack& Track : OutTracks.Tracks)
	{
		Track.KeyFrames.Sort();

		for (int32 KeyFrameIndex = Track.KeyFrames.Num() - 1; KeyFrameIndex > 0; --KeyFrameIndex)
		{
			if (Track.KeyFrames[KeyFrameIndex].Time == Track.KeyFrames[KeyFrameIndex - 1].Time)
			{
				Track.KeyFrames.RemoveAt(KeyFrameIndex);
			}
		}

		if (Track.KeyFrames.IsEmpty())
		{
			continue;
		}

		if (!FMath::IsNearlyZero(Track.KeyFrames[0].Time))
		{
			FKeyFrame ZeroFrame = Track.KeyFrames[0];
			ZeroFrame.Time = 0.0;
			Track.KeyFrames.Insert(ZeroFrame, 0);
		}
		else
		{
			Track.KeyFrames[0].Time = 0.0;
		}
	}

	OutTracks.IsReady = true;
}

UAnimSequence* FRuntimeAnimationGenerator::GenerateSkeletonAnimSequence(
	USkeleton* Skeleton,
	const FTracks& TracksContainer,
	UObject* Outer)
{
	if (Skeleton == nullptr)
	{
		UE_LOG(LogRuntimeAnimationGenerator, Warning, TEXT("GenerateSkeletonAnimSequence requires a valid skeleton."));
		return nullptr;
	}

	if (!TracksContainer.IsReady)
	{
		UE_LOG(LogRuntimeAnimationGenerator, Warning, TEXT("GenerateSkeletonAnimSequence requires PrepareSkeletonTracks to be called first."));
		return nullptr;
	}

	const TArray<FTrack>& Tracks = TracksContainer.Tracks;
	if (Tracks.IsEmpty())
	{
		return nullptr;
	}

#if !WITH_EDITOR
	UE_LOG(LogRuntimeAnimationGenerator, Warning, TEXT("GenerateSkeletonAnimSequence is only supported in editor builds."));
	return nullptr;
#else
	UObject* EffectiveOuter = Outer != nullptr ? Outer : GetTransientPackage();
	UAnimSequence* Anim = NewObject<UAnimSequence>(EffectiveOuter);
	if (Anim == nullptr)
	{
		return nullptr;
	}

	Anim->BoneCompressionSettings = FAnimationUtils::GetDefaultAnimationRecorderBoneCompressionSettings();
	Anim->SetSkeleton(Skeleton);

	double SequenceDuration = 0.0;
	const double FrameIntervalSeconds = ResolveFrameIntervalAndDuration(Tracks, SequenceDuration);
	const int32 NumKeys = SequenceDuration > 0.0
		? FMath::Max(1, FMath::CeilToInt(SequenceDuration / FrameIntervalSeconds) + 1)
		: 1;
	const int32 NumFrames = FMath::Max(0, NumKeys - 1);
	const FFrameRate FrameRate = MakeFrameRateFromInterval(FrameIntervalSeconds);

	IAnimationDataController& Controller = Anim->GetController();
	IAnimationDataController::FScopedBracket ScopedBracket(
		Controller,
		FText::FromString(TEXT("Generating runtime animation sequence")),
		false);

	Controller.InitializeModel();
	Controller.SetFrameRate(FrameRate, false);
	Controller.SetNumberOfFrames(FFrameNumber(NumFrames), false);
	Anim->InitializeNotifyTrack();

	for (const FTrack& Track : Tracks)
	{
		TArray<FVector3f> PosKeys;
		TArray<FQuat4f> RotKeys;
		TArray<FVector3f> ScaleKeys;
		BuildUniformTrackKeys(Track, NumKeys, FrameIntervalSeconds, PosKeys, RotKeys, ScaleKeys);

		if (!Controller.AddBoneCurve(Track.BoneName, false))
		{
			UE_LOG(LogRuntimeAnimationGenerator, Warning, TEXT("Failed to add bone curve '%s'."), *Track.BoneName.ToString());
			return nullptr;
		}

		if (!Controller.SetBoneTrackKeys(Track.BoneName, PosKeys, RotKeys, ScaleKeys, false))
		{
			UE_LOG(LogRuntimeAnimationGenerator, Warning, TEXT("Failed to set keys for bone curve '%s'."), *Track.BoneName.ToString());
			return nullptr;
		}
	}

	Controller.NotifyPopulated();

	if (Anim->GetOutermost() != GetTransientPackage())
	{
		Anim->CacheDerivedDataForCurrentPlatform();
		Anim->MarkPackageDirty();
	}

	return Anim;
#endif
}
