#include "pch.h"  // RE/Skyrim.h + SKSE; xmake does not force-include the PCH like CMake does

// TooManyNotificationRemade
//
// Vanilla hudmenu.swf (class `Messages`) shows at most Messages.MAX_SHOWN notifications.
// Once that limit is reached, new notifications wait in Messages.MessageArray until the
// oldest one has fully faded out, so a burst of notifications shows up long after the
// event that caused it.
//
// This plugin patches the HUD movie at runtime, so hudmenu.swf is never overridden:
//  - `Messages.prototype.Update` is wrapped by a native function.
//  - Each Messages instance gets an invisible "ticker" clip whose onEnterFrame runs every frame.
//  - When notifications are queued and the newest one fills the last free slot, the oldest shown
//    notification is taken out of the Messages bookkeeping as soon as the slide starts, before it
//    reaches the last slot. The next queued notification can then start the moment the slide
//    ends, with no pause.
//  - The removed notification fades out over kFadeSeconds and keeps moving down with the others
//    while it fades, then it is deleted.

namespace
{
	constexpr double kFadeSeconds = 0.25;

	constexpr auto kOriginalUpdate = "_tmnOriginalUpdate";
	constexpr auto kTickFunction = "_tmnTick";
	constexpr auto kFadeFunction = "_tmnFade";
	constexpr auto kTickerName = "_tmnTicker";

	constexpr auto kMessagesClass = "_global.Messages";
	constexpr auto kMaxShownPath = "_global.Messages.MAX_SHOWN";

	constexpr int kMaxInstallAttempts = 600;  // about 10 seconds of UI frames

	double Now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	std::optional<double> GetNumber(const RE::GFxValue& a_obj, const char* a_name)
	{
		RE::GFxValue value;
		if (a_obj.GetMember(a_name, &value) && value.IsNumber()) {
			return value.GetNumber();
		}
		return std::nullopt;
	}

	std::optional<double> GetGlobalNumber(RE::GFxMovie* a_movie, const char* a_path)
	{
		RE::GFxValue value;
		if (a_movie && a_movie->GetVariable(&value, a_path) && value.IsNumber()) {
			return value.GetNumber();
		}
		return std::nullopt;
	}

	bool GetBool(const RE::GFxValue& a_obj, const char* a_name)
	{
		RE::GFxValue value;
		return a_obj.GetMember(a_name, &value) && value.IsBool() && value.GetBool();
	}

	bool IsDefined(const RE::GFxValue& a_value)
	{
		return !a_value.IsUndefined() && !a_value.IsNull();
	}

	// Fade-out of a notification that has already been removed from the Messages arrays.
	// Runs as that clip's onEnterFrame.
	class FadeHandler final : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			if (!a_params.thisPtr || !a_params.thisPtr->IsDisplayObject()) {
				return;
			}
			auto& clip = *a_params.thisPtr;

			// Follow the notification that was above this one, so this clip keeps moving down
			// with the stack while the next notification slides in.
			RE::GFxValue ref;
			if (clip.GetMember("_tmnRef", &ref) && ref.IsDisplayObject()) {
				const auto refY = GetNumber(ref, "_y");
				const auto lastRefY = GetNumber(clip, "_tmnRefY");
				const auto y = GetNumber(clip, "_y");
				if (refY && lastRefY && y) {
					clip.SetMember("_y", RE::GFxValue(*y + (*refY - *lastRefY)));
					clip.SetMember("_tmnRefY", RE::GFxValue(*refY));
				}
			}

			const auto start = GetNumber(clip, "_tmnFadeStart").value_or(0.0);
			const auto from = GetNumber(clip, "_tmnFadeFrom").value_or(100.0);
			const auto t = (Now() - start) / kFadeSeconds;

			if (t >= 1.0) {
				clip.Invoke("removeMovieClip");
				return;
			}
			clip.SetMember("_alpha", RE::GFxValue(from * (1.0 - t)));
		}
	};

	void StartFadeOut(RE::GFxValue& a_clip, const RE::GFxValue* a_ref, const RE::GFxValue& a_fadeFn)
	{
		a_clip.Invoke("stop");  // freeze its own timeline (vanilla FadeOut / END_ANIM_FRAME no longer matter)
		a_clip.SetMember("_tmnFadeStart", RE::GFxValue(Now()));
		a_clip.SetMember("_tmnFadeFrom", RE::GFxValue(GetNumber(a_clip, "_alpha").value_or(100.0)));

		if (a_ref && a_ref->IsDisplayObject()) {
			if (const auto refY = GetNumber(*a_ref, "_y")) {
				a_clip.SetMember("_tmnRef", *a_ref);
				a_clip.SetMember("_tmnRefY", RE::GFxValue(*refY));
			}
		}

		a_clip.SetMember("onEnterFrame", a_fadeFn);
	}

	// Takes the oldest shown notification (ShownMessageArray[0]) out of the Messages
	// bookkeeping and fades it out on its own.
	bool DetachOldest(RE::GFxValue& a_messages, RE::GFxValue& a_shown, double a_shownCount)
	{
		RE::GFxValue fadeFn;
		if (!a_messages.GetMember(kFadeFunction, &fadeFn) || !IsDefined(fadeFn)) {
			return false;
		}

		RE::GFxValue oldest;
		if (!a_shown.GetElement(0, &oldest)) {
			return false;
		}
		a_shown.RemoveElement(0);
		a_messages.SetMember("ShownCount", RE::GFxValue(a_shownCount - 1.0));

		if (oldest.IsDisplayObject()) {
			RE::GFxValue next;
			const bool hasNext = a_shown.GetArraySize() > 0 && a_shown.GetElement(0, &next);
			StartFadeOut(oldest, hasNext ? &next : nullptr, fadeFn);
		}
		return true;
	}

	// Runs every frame (ticker) and before every Messages.Update.
	void Process(RE::GFxMovie* a_movie, RE::GFxValue& a_messages)
	{
		const auto maxShown = GetGlobalNumber(a_movie, kMaxShownPath);
		if (!maxShown) {
			return;
		}

		RE::GFxValue queued;
		if (!a_messages.GetMember("MessageArray", &queued) || !queued.IsArray() || queued.GetArraySize() == 0) {
			return;  // nothing waiting, so the vanilla behaviour is fine
		}

		const auto shownCount = GetNumber(a_messages, "ShownCount").value_or(0.0);
		if (shownCount < *maxShown) {
			return;
		}

		RE::GFxValue shown;
		if (!a_messages.GetMember("ShownMessageArray", &shown) || !shown.IsArray() || shown.GetArraySize() == 0) {
			return;
		}

		// While a notification slides in, Messages has an onEnterFrame that counts ySpacing up to Y_SPACING.
		RE::GFxValue enterFrame;
		const bool sliding = a_messages.GetMember("onEnterFrame", &enterFrame) && IsDefined(enterFrame);

		if (sliding) {
			// The newest notification just filled the last free slot and is pushing the oldest
			// toward the last slot. Start removing the oldest right away, so it fades out while
			// it moves and never settles there.
			// The slide's own onEnterFrame clears bAnimating when it ends; leave it alone.
			DetachOldest(a_messages, shown, shownCount);
		} else {
			// Not sliding: either idle at the limit or vanilla is fading the oldest out
			// (which keeps bAnimating set). Take the oldest over and let the next one in now.
			if (DetachOldest(a_messages, shown, shownCount)) {
				a_messages.SetMember("bAnimating", RE::GFxValue(false));
			}
		}
	}

	// Gives the Messages instance an empty child clip whose onEnterFrame runs Process every frame.
	void EnsureTicker(RE::GFxValue& a_messages)
	{
		if (!a_messages.IsDisplayObject()) {
			return;
		}

		RE::GFxValue ticker;
		if (a_messages.GetMember(kTickerName, &ticker) && ticker.IsDisplayObject()) {
			return;
		}

		RE::GFxValue tickFn;
		if (!a_messages.GetMember(kTickFunction, &tickFn) || !IsDefined(tickFn)) {
			return;
		}

		RE::GFxValue depth;
		if (!a_messages.Invoke("getNextHighestDepth", &depth) || !depth.IsNumber()) {
			return;
		}

		const std::array<RE::GFxValue, 2> args{ RE::GFxValue(kTickerName), depth };
		if (a_messages.Invoke("createEmptyMovieClip", &ticker, args) && ticker.IsDisplayObject()) {
			ticker.SetMember("onEnterFrame", tickFn);
		}
	}

	class TickHandler final : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			if (!a_params.thisPtr || !a_params.thisPtr->IsDisplayObject()) {
				return;
			}

			RE::GFxValue messages;
			if (a_params.thisPtr->GetMember("_parent", &messages) && messages.IsDisplayObject()) {
				Process(a_params.movie, messages);
			}
		}
	};

	class UpdateHandler final : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			if (!a_params.thisPtr || !a_params.thisPtr->IsObject()) {
				return;
			}

			auto& messages = *a_params.thisPtr;
			EnsureTicker(messages);
			Process(a_params.movie, messages);

			// Call the original Messages.Update with the same arguments.
			messages.Invoke(kOriginalUpdate, a_params.retVal, a_params.args, a_params.argCount);
		}
	};

	template <class T>
	RE::GFxValue MakeFunction(RE::GFxMovieView* a_movie)
	{
		// The movie holds its own reference to the handler; drop ours.
		auto handler = new T();
		RE::GFxValue fn;
		a_movie->CreateFunction(&fn, handler);
		handler->Release();
		return fn;
	}

	enum class InstallResult
	{
		kInstalled,
		kAlreadyInstalled,
		kNotReady
	};

	InstallResult TryInstall()
	{
		auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return InstallResult::kNotReady;
		}

		auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME);
		if (!hud || !hud->uiMovie) {
			return InstallResult::kNotReady;
		}
		auto movie = hud->uiMovie.get();

		RE::GFxValue messagesClass;
		if (!movie->GetVariable(&messagesClass, kMessagesClass) || !messagesClass.IsObject()) {
			return InstallResult::kNotReady;  // classes not registered yet (first frame not run)
		}

		RE::GFxValue prototype;
		if (!messagesClass.GetMember("prototype", &prototype) || !prototype.IsObject()) {
			return InstallResult::kNotReady;
		}

		if (prototype.HasMember(kOriginalUpdate)) {
			return InstallResult::kAlreadyInstalled;
		}

		RE::GFxValue originalUpdate;
		if (!prototype.GetMember("Update", &originalUpdate) || !IsDefined(originalUpdate)) {
			return InstallResult::kNotReady;
		}

		prototype.SetMember(kTickFunction, MakeFunction<TickHandler>(movie));
		prototype.SetMember(kFadeFunction, MakeFunction<FadeHandler>(movie));
		prototype.SetMember(kOriginalUpdate, originalUpdate);
		prototype.SetMember("Update", MakeFunction<UpdateHandler>(movie));
		return InstallResult::kInstalled;
	}

	void ScheduleInstall(int a_attempt)
	{
		auto task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		task->AddUITask([a_attempt]() {
			switch (TryInstall()) {
			case InstallResult::kInstalled:
				SKSE::log::info("Patched Messages.Update in the HUD movie");
				break;
			case InstallResult::kAlreadyInstalled:
				break;
			case InstallResult::kNotReady:
				if (a_attempt + 1 < kMaxInstallAttempts) {
					ScheduleInstall(a_attempt + 1);
				} else {
					SKSE::log::warn("Could not find the Messages class in the HUD movie; notifications are not patched");
				}
				break;
			}
		});
	}

	class MenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuWatcher* GetSingleton()
		{
			static MenuWatcher singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			// The HUD movie can be (re)created, for example when loading a save, so check every time it opens.
			if (a_event && a_event->opening && a_event->menuName == RE::HUDMenu::MENU_NAME) {
				ScheduleInstall(0);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			if (auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::GetSingleton());
			}
			ScheduleInstall(0);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			ScheduleInstall(0);
			break;
		default:
			break;
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* skse)
{
	if (!skse) {
		return false;
	}
	SKSE::Init(skse);

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		return false;
	}
	return true;
}
