#include "retarget/custom_intent.hpp"

#include <cstdlib>
#include <iostream>

using namespace hd2aa::retarget;

namespace
{
int failures = 0;

void check(bool value, const char *message)
{
	if (!value)
	{
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
}
}

int main()
{
	custom_intent_mailbox intent;
	check(!intent.load().desired && intent.load().revision == 0,
		"mailbox starts disabled");

	check(intent.toggle(), "first F8 edge requests enable");
	const custom_intent_snapshot enabled = intent.load();
	check(enabled.desired && enabled.revision == 1,
		"worker observes enabled revision");

	check(!intent.toggle(), "second F8 edge requests disable");
	check(intent.toggle(), "third F8 edge supersedes disable");
	const custom_intent_snapshot coalesced = intent.load();
	check(coalesced.desired && coalesced.revision == 3,
		"worker can consume the latest intent without replaying obsolete work");

	intent.request(false);
	const custom_intent_snapshot stopped = intent.load();
	check(!stopped.desired && stopped.revision == 4,
		"shutdown publishes one disable revision");
	intent.request(false);
	check(intent.load().revision == 4,
		"repeating the same desired state creates no work");

	intent.reset();
	check(!intent.load().desired && intent.load().revision == 0,
		"mailbox resets between device lifetimes");

	if (failures == 0)
		std::cout << "custom runtime intent mailbox passed\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
