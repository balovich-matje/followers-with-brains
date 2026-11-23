#pragma once

// Movement driver: for a short moment the plugin moves a follower's body itself (position, heading,
// physics velocity) along a planned path, and makes the legs animate to match (speed, direction and
// turn channels of the animation graph). The combat AI's own movement and attacks are held off
// while a follower is driven. Used for moves the combat AI won't make on request: a weaving sprint
// at a shooter, a quick step back out of a power attack, turning and running with the player.
// Works on any runtime the plugin loads on (no modpack-specific data).
namespace Driver
{
	enum class Kind
	{
		kCharge,    // run at the target, weaving side to side, facing it
		kStepBack,  // back away from the target, facing it
		kRunTo,     // run toward the target (e.g. the player), facing the way it runs
		kFormation,  // hold an area next to the target (the player) while it moves
		kHop         // jump to the landing point (up a rock, down a ledge, over a fence)
	};

	struct Order
	{
		Kind            kind = Kind::kCharge;
		RE::ActorHandle target;
		float           speed = 450.0f;        // units per second
		float           duration = 10.0f;      // seconds, at most
		float           stopDistance = 450.0f;  // charge/run: done this close; step back: done this far
		bool            sprint = false;        // show the sprint animation if the graph has one
		bool            holdWhileAttacked = false;  // step back: stay out until the target's swing is over
		float           weave = 1.0f;          // charge: sideways share (1 = 45 degrees); step back: 1 = diagonal
		float           flipMin = 0.25f;       // charge: seconds between side changes
		float           flipMax = 0.6f;
		// formation: an area around the target, in the target's frame (angle 0 = ahead, +90 = right)
		float           zoneAngle = 180.0f;    // degrees: centre of the area (behind)
		float           zoneHalf = 40.0f;      // degrees either side of the centre
		float           zoneNear = 120.0f;     // units from the target
		float           zoneFar = 320.0f;
		bool            zoneMirror = false;    // side placement: either side, whichever the follower is on
		float           sprintSpeed = 715.0f;  // formation: top speed while the target sprints
		RE::NiPoint3    landing;               // hop: where to land
		float           looseness = 1.0f;      // formation: 1 tight; larger = bigger area, softer pull
	};

	bool Install();  // animation channel and action hooks (kDataLoaded)
	void Frame(float a_delta);  // every frame, main thread

	bool Start(RE::Actor* a_actor, const Order& a_order);
	void Stop(RE::Actor* a_actor);
	void StopAll();
	bool IsDriving(RE::Actor* a_actor);
	void SetLooseness(RE::Actor* a_actor, float a_looseness);  // formation in progress

	// True when nothing solid (terrain, walls, rocks, fences) lies on the straight line between the
	// two points, at chest height. People are ignored.
	bool ClearLine(RE::Actor* a_actor, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);

	// Formation area helpers: where in a_target's frame a_actor stands, and the nearest point of the
	// order's area (in world space). Returns the distance to the area (0 inside).
	// A landing spot near a_point on solid ground: the ground height under it (within a_range up or
	// down of a_point.z), or false when there is no ground there.
	bool GroundAt(RE::Actor* a_actor, RE::NiPoint3& a_point, float a_range);

	float ZoneGap(const Order& a_order, RE::Actor* a_actor, RE::Actor* a_target, RE::NiPoint3* a_nearest = nullptr);

	// Why the last drive of this actor ended (for logs and tests).
	const char* LastEnd(RE::Actor* a_actor);
}
