// Renderer options expose every choice. Availability controls activation,
// independently of focus, so mouse and controller users can inspect failures.
class OptionMenuItemSupportedOption : OptionMenuItemOption
{
	static native String UnavailableReason(Name setting, int value);

	OptionMenuItemSupportedOption Init(String label, Name command, Name values)
	{
		Super.Init(label, command, values);
		return self;
	}

	String ChoiceReason(int choice)
	{
		return UnavailableReason(mAction, int(OptionValues.GetValue(mValues, choice)));
	}

	override bool Selectable() { return true; }

	override bool isGrayed()
	{
		if (mAction == 'nri_ptsmoke') return UnavailableReason(mAction, 1).Length() > 0;
		if (mAction == 'nri_ptsmokeworkprofile') return ChoiceReason(0).Length() > 0;
		return false;
	}

	void ApplyChoice(int choice)
	{
		// Recheck after the menu was opened: the display/backend may have changed.
		String reason = ChoiceReason(choice);
		if (reason.Length() > 0)
		{
			Menu.StartMessage(reason, 1);
			return;
		}
		Super.SetSelection(choice);
		Menu.MenuSound("menu/change");
		Menu.GetCurrentMenu().Close();
		if (mAction == 'nri_settingsprofile' && choice != 0 && GetSelection() == choice)
		{
			String hdrReason = UnavailableReason('nri_ptoutputmode', 1);
			if (hdrReason.Length() > 0)
				Menu.StartMessage("Profile applied using SDR.\n\n" .. hdrReason, 1);
		}
	}

	override bool Activate()
	{
		let desc = OptionMenuDescriptor(MenuDescriptor.GetDescriptor('SupportedSettingChoices'));
		if (desc == null) return false;
		desc.mItems.Clear();
		desc.mTitle = mLabel;
		desc.mScrollPos = 0;
		desc.mSelectedItem = max(GetSelection(), 0);
		desc.mIndent = 0;
		for (int i = 0; i < OptionValues.GetCount(mValues); ++i)
		{
			let item = new('OptionMenuItemSupportedChoice').Init(
				StringTable.Localize(OptionValues.GetText(mValues, i)), self, i);
			desc.mItems.Push(item);
		}
		Menu.MenuSound("menu/advance");
		Menu.SetMenu('SupportedSettingChoices');
		return true;
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		if (mkey == Menu.MKEY_Left || mkey == Menu.MKEY_Right || mkey == Menu.MKEY_Enter)
			return Activate();
		return false;
	}
}

class OptionMenuItemSupportedChoice : OptionMenuItem
{
	OptionMenuItemSupportedOption mSetting;
	int mChoice;

	OptionMenuItemSupportedChoice Init(String label, OptionMenuItemSupportedOption setting, int choice)
	{
		Super.Init(label, 'none');
		mSetting = setting;
		mChoice = choice;
		return self;
	}

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		bool unavailable = mSetting.ChoiceReason(mChoice).Length() > 0;
		drawLabel(indent, y, selected ? OptionMenuSettings.mFontColorSelection :
			OptionMenuSettings.mFontColor, unavailable);
		drawValue(indent, y, OptionMenuSettings.mFontColorValue,
			unavailable ? "Unavailable" : (mSetting.GetSelection() == mChoice ? "Active" : ""), unavailable);
		return indent;
	}

	override bool Activate()
	{
		mSetting.ApplyChoice(mChoice);
		return true;
	}
}
