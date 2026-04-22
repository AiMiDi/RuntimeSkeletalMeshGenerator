#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "RuntimeAnimationGenerator.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"

namespace
{
USkeleton* CreateTestSkeleton()
{
	USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);
	FReferenceSkeletonModifier ReferenceSkeletonModifier(Skeleton);
	ReferenceSkeletonModifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	ReferenceSkeletonModifier.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(0.0, 10.0, 0.0)));
	return Skeleton;
}

FRuntimeAnimationGenerator::FKeyFrame MakeKey(
	const double Time,
	const FVector& Position = FVector::ZeroVector,
	const FQuat& Rotation = FQuat::Identity,
	const FVector& Scale = FVector::OneVector)
{
	return FRuntimeAnimationGenerator::FKeyFrame(Time, Position, Rotation, Scale);
}
} // namespace

BEGIN_DEFINE_SPEC(
	FRuntimeAnimationGeneratorSpec,
	"RuntimeSkeletalMeshGenerator.Unit.Animation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FRuntimeAnimationGeneratorSpec)

void FRuntimeAnimationGeneratorSpec::Define()
{
	Describe("PrepareSkeletonTracks", [this]()
	{
		It("sorts, deduplicates, removes invalid tracks, and injects frame zero", [this]()
		{
			USkeleton* Skeleton = CreateTestSkeleton();

			FRuntimeAnimationGenerator::FTracks Tracks;
			TArray<FRuntimeAnimationGenerator::FTrack>& MutableTracks = Tracks.GetTracks_mutable();

			FRuntimeAnimationGenerator::FTrack RootTrack;
			RootTrack.BoneName = TEXT("root");
			RootTrack.KeyFrames = {
				MakeKey(1.0, FVector(10.0, 0.0, 0.0)),
				MakeKey(0.5, FVector(5.0, 0.0, 0.0)),
				MakeKey(1.0, FVector(20.0, 0.0, 0.0)),
			};
			MutableTracks.Add(RootTrack);

			FRuntimeAnimationGenerator::FTrack ChildTrack;
			ChildTrack.BoneName = TEXT("child");
			ChildTrack.KeyFrames = {MakeKey(0.0, FVector(0.0, 5.0, 0.0))};
			MutableTracks.Add(ChildTrack);

			FRuntimeAnimationGenerator::FTrack InvalidTrack;
			InvalidTrack.BoneName = TEXT("missing");
			InvalidTrack.KeyFrames = {MakeKey(0.0)};
			MutableTracks.Add(InvalidTrack);

			FRuntimeAnimationGenerator::FTrack EmptyTrack;
			EmptyTrack.BoneName = TEXT("root");
			MutableTracks.Add(EmptyTrack);

			FRuntimeAnimationGenerator::PrepareSkeletonTracks(Skeleton, Tracks);

			const TArray<FRuntimeAnimationGenerator::FTrack>& PreparedTracks = Tracks.GetTracks();
			TestTrue(TEXT("Tracks are marked ready"), Tracks.GetIsReady());
			TestEqual(TEXT("Only valid, non-empty tracks remain"), PreparedTracks.Num(), 2);

			const FRuntimeAnimationGenerator::FTrack* PreparedRootTrack =
				PreparedTracks.FindByPredicate([](const FRuntimeAnimationGenerator::FTrack& Track)
				{
					return Track.BoneName == TEXT("root");
				});
			TestNotNull(TEXT("Root track exists"), PreparedRootTrack);
			TestEqual(TEXT("Root track has deduplicated keys"), PreparedRootTrack->KeyFrames.Num(), 3);
			TestTrue(TEXT("Frame zero was inserted"), FMath::IsNearlyZero(PreparedRootTrack->KeyFrames[0].Time));
			TestTrue(TEXT("Second key time is sorted"), FMath::IsNearlyEqual(PreparedRootTrack->KeyFrames[1].Time, 0.5));
			TestTrue(TEXT("Third key time is sorted"), FMath::IsNearlyEqual(PreparedRootTrack->KeyFrames[2].Time, 1.0));

			const FRuntimeAnimationGenerator::FTrack* PreparedChildTrack =
				PreparedTracks.FindByPredicate([](const FRuntimeAnimationGenerator::FTrack& Track)
				{
					return Track.BoneName == TEXT("child");
				});
			TestNotNull(TEXT("Child track exists"), PreparedChildTrack);
			TestEqual(TEXT("Child track still has a single key"), PreparedChildTrack->KeyFrames.Num(), 1);
			TestTrue(TEXT("Child track remains at frame zero"), FMath::IsNearlyZero(PreparedChildTrack->KeyFrames[0].Time));
		});
	});

	Describe("GenerateSkeletonAnimSequence", [this]()
	{
		It("creates an anim sequence via the UE5.3 animation data controller", [this]()
		{
			USkeleton* Skeleton = CreateTestSkeleton();

			FRuntimeAnimationGenerator::FTracks Tracks;
			TArray<FRuntimeAnimationGenerator::FTrack>& MutableTracks = Tracks.GetTracks_mutable();

			FRuntimeAnimationGenerator::FTrack RootTrack;
			RootTrack.BoneName = TEXT("root");
			RootTrack.KeyFrames = {
				MakeKey(0.0, FVector::ZeroVector),
				MakeKey(0.5, FVector(10.0, 0.0, 0.0)),
			};
			MutableTracks.Add(RootTrack);

			FRuntimeAnimationGenerator::FTrack ChildTrack;
			ChildTrack.BoneName = TEXT("child");
			ChildTrack.KeyFrames = {MakeKey(0.0, FVector(0.0, 5.0, 0.0))};
			MutableTracks.Add(ChildTrack);

			FRuntimeAnimationGenerator::PrepareSkeletonTracks(Skeleton, Tracks);

			UAnimSequence* AnimSequence =
				FRuntimeAnimationGenerator::GenerateSkeletonAnimSequence(Skeleton, Tracks, GetTransientPackage());
			TestNotNull(TEXT("Anim sequence is created"), AnimSequence);
			TestEqual(TEXT("Skeleton is assigned"), AnimSequence->GetSkeleton(), Skeleton);
			TestEqual(TEXT("Two sampled keys were generated"), AnimSequence->GetNumberOfSampledKeys(), 2);
			TestTrue(TEXT("Play length is preserved"), FMath::IsNearlyEqual(AnimSequence->GetPlayLength(), 0.5f));

			const IAnimationDataModel* DataModel = AnimSequence->GetDataModel();
			TestNotNull(TEXT("Animation data model exists"), DataModel);

			TArray<FName> BoneTrackNames;
			DataModel->GetBoneTrackNames(BoneTrackNames);
			TestEqual(TEXT("Two bone tracks are authored"), BoneTrackNames.Num(), 2);
			TestTrue(TEXT("Root track exists"), BoneTrackNames.Contains(TEXT("root")));
			TestTrue(TEXT("Child track exists"), BoneTrackNames.Contains(TEXT("child")));
			TestEqual(TEXT("Frame count matches the last frame index"), DataModel->GetNumberOfFrames(), 1);
			TestTrue(TEXT("Frame rate matches the smallest interval"), FMath::IsNearlyEqual(DataModel->GetFrameRate().AsDecimal(), 2.0));
		});
	});
}

#endif
