/******************************************************************************/
/* Animation generation utilities for UE5.3                                   */
/* -------------------------------------------------------------------------- */
/* License MIT                                                                */
/* Kindly sponsored by IMVU                                                   */
/* -------------------------------------------------------------------------- */
/* Runtime helpers to prepare animation tracks and build a transient sequence.*/
/******************************************************************************/
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UAnimSequence;
class UObject;
class USkeleton;

class RUNTIMEANIMATIONGENERATOR_API FRuntimeAnimationGeneratorModule : public IModuleInterface
{
public: // ------------------------------------- IModuleInterface implementation
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};


class RUNTIMEANIMATIONGENERATOR_API FRuntimeAnimationGenerator
{
public:
	struct RUNTIMEANIMATIONGENERATOR_API FKeyFrame
	{
		double Time;
		FVector Position;
		FQuat Rotation;
		FVector Scale;

		explicit FKeyFrame (const double Time = 0.0,
		                    const FVector& Position = FVector::ZeroVector,
		                    const FQuat& Rotation = FQuat::Identity,
		                    const FVector& Scale = FVector::OneVector);

		bool operator<(const FKeyFrame& OtherKey) const
		{
			return Time < OtherKey.Time;
		}
	};

	struct RUNTIMEANIMATIONGENERATOR_API FTrack
	{
		FName BoneName;
		TArray<FKeyFrame> KeyFrames;

		bool operator==(const FName& Name) const
		{
			return Name == BoneName;
		}
	};

	class RUNTIMEANIMATIONGENERATOR_API FTracks
	{
		friend class FRuntimeAnimationGenerator;

		bool IsReady = false;
		TArray<FTrack> Tracks;

	public:
		bool GetIsReady() const
		{
			return IsReady;
		}

		TArray<FTrack>& GetTracks_mutable()
		{
			IsReady = false;
			return Tracks;
		}

		const TArray<FTrack>& GetTracks() const
		{
			return Tracks;
		}
	};

public:
	static void PrepareSkeletonTracks(const USkeleton* Skeleton, FTracks& OutTracks);

	/// Generates a new `AnimSequence` using the passed Tracks.
	/// Note, it's important to use `PrepareTracks` just before using this function.
	static UAnimSequence* GenerateSkeletonAnimSequence(USkeleton* Skeleton, const FTracks& Tracks, UObject* Outer = GetTransientPackage());
};
