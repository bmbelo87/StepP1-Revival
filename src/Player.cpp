#include "global.h"
#include "Player.h"
#include "GameConstantsAndTypes.h"
#include "RageUtil.h"
#include "RageTimer.h"
#include "PrefsManager.h"
#include "GameManager.h"
#include "InputMapper.h"
#include "SongManager.h"
#include "GameState.h"
#include "ScoreKeeperNormal.h"
#include "RageLog.h"
#include "RageDisplay.h"
#include "ThemeManager.h"
#include "ScoreDisplay.h"
#include "LifeMeter.h"
#include "CombinedLifeMeter.h"
#include "NoteField.h"
#include "NoteDataUtil.h"
#include "ScreenMessage.h"
#include "ScreenManager.h"
#include "StageStats.h"
#include "ActorUtil.h"
#include "ArrowEffects.h"
#include "Game.h"
#include "NetworkSyncManager.h"	//used for sending timing offset 
#include "DancingCharacters.h"
#include "ScreenDimensions.h"
#include "RageSoundManager.h"
#include "ThemeMetric.h"
#include "PlayerState.h"
#include "GameSoundManager.h"
#include "Style.h"
#include "MessageManager.h"
#include "ProfileManager.h"
#include "Profile.h"
#include "StatsManager.h"
#include "Song.h"
#include "Steps.h"
#include "GameCommand.h"
#include "LocalizedString.h"
#include "AdjustSync.h"

static Preference<float> m_fTimingWindowJump		("TimingWindowJump", 0.25);
static Preference<float> m_fTimingWindowHold		("TimingWindowHold", 0.25);
static Preference<float> m_fMaxInputLatencySeconds	("MaxInputLatencySeconds", 0.0);
static Preference<bool>  g_bEnableMineSoundPlayback	("EnableMineHitSound", true);

ThemeMetric<float>	ATTACK_RUN_TIME_RANDOM		("Player", "AttackRunTimeRandom");
ThemeMetric<float>	ATTACK_RUN_TIME_MINE		("Player", "AttackRunTimeMine");
ThemeMetric<float>	MAX_HOLD_LIFE			("Player", "MaxHoldLife");
ThemeMetric<float>	M_MOD_HIGH_CAP			("Player", "MModHighCap");
ThemeMetric<bool>	BATTLE_RAVE_MIRROR		("Player", "BattleRaveMirror");

/* xMAx - PIU Timings ----------------------------------------------------*/
// iPerfect;  iDelay;  iDelta;
static const Player::JudgeData SJ = { 10, 5, 5 };	// Only for Basic Mode
static const Player::JudgeData EJ = { 7, 5, 5 };
static const Player::JudgeData NJ = { 5, 5, 5 };
static const Player::JudgeData HJ = { 3, 5, 5 };
static const Player::JudgeData VJ = { 1, 5, 4 };
static const Player::JudgeData XJ = { 0, 5, 2 };
static const Player::JudgeData UJ = { 0, 5, 1 };

Player::JudgeData &Player::JudgeData::operator=( const JudgeData &judgeData )
{
	iPerfect = judgeData.iPerfect;
	iDelay = judgeData.iDelay;
	iDelta = judgeData.iDelta;
	return *this;
}
/*------------------------------------------------------------------------------------*/

float Player::GetWindowSeconds ( TimingWindow tw )
{
	float fSecs = 0;
	switch ( tw )
	{
	case TW_Mine:	fSecs = GOOD_U;		break;	//same as good top	
	case TW_Attack: fSecs = GREAT_U;	break;	//same as great top
	case TW_Hold:	fSecs = 0.23f;		break;	//allow enough time to take foot off and put back on
	case TW_Roll:	fSecs = 0.350f;		break;
	default: break;
	}

	return fSecs;
}

Player::Player( NoteData &nd, bool bVisibleParts ) : m_NoteData(nd)
{
	m_bLoaded = false;

	// Imported stuff
	m_pPlayerState = NULL;
	m_pPlayerStageStats = NULL;
	m_pLifeMeter = NULL;
	m_pCombinedLifeMeter = NULL;
	m_pScoreDisplay = NULL;
	m_pSecondaryScoreDisplay = NULL;
	m_pPrimaryScoreKeeper = NULL;
	m_pSecondaryScoreKeeper = NULL;
	m_pInventory = NULL;

	// Local stuff
	m_pIterNeedsTapJudging = NULL;
	m_pIterUncrossedRows = NULL;
	m_pIterNeedsHoldJudging = NULL;
	m_pNoteField = NULL;

	m_bPaused = false;
	m_bDelay = false;

	if( bVisibleParts )
	{
		m_pNoteField = new NoteField;
		m_pNoteField->SetName( "NoteField" );
	}

	m_bSendJudgmentAndComboMessages = true;
	m_bCountNotesSeparately = false;
	// xMAx ----------------------------------------
	PERF_U = 0;
	PERF_D = 0;
	GREAT_U = 0;
	GREAT_D = 0;
	GOOD_U = 0;
	GOOD_D = 0;
	BAD_U = 0;
	BAD_D = 0;
	HOLD_TIMING = 0;
	// ----------------------------------------------
}

Player::~Player()
{
	if ( m_pPlayerState )
	{
		m_pPlayerState->m_CacheDisplayedBeat.clear();
		m_pPlayerState->m_CacheNoteStat.clear();
	}

	SAFE_DELETE( m_pNoteField );
	SAFE_DELETE( m_pIterNeedsTapJudging );
	SAFE_DELETE ( m_pIterUncrossedRows );
	SAFE_DELETE( m_pIterNeedsHoldJudging );
}


void RoundUpToTwoDecimal ( float &toRound )
{
	float temp = toRound;
	toRound = ((ceil(temp*1000))/1000.0f);
}


/* Init() does the expensive stuff: load sounds and noteskins.  Load() just loads a NoteData. */
void Player::Init(
	const RString &sType,
	PlayerState* pPlayerState, 
	PlayerStageStats* pPlayerStageStats,
	LifeMeter* pLM, 
	CombinedLifeMeter* pCombinedLM, 
	ScoreDisplay* pScoreDisplay, 
	ScoreDisplay* pSecondaryScoreDisplay, 
	Inventory* pInventory, 
	ScoreKeeper* pPrimaryScoreKeeper, 
	ScoreKeeper* pSecondaryScoreKeeper )
{
	DRAW_DISTANCE_AFTER_TARGET_PIXELS.Load(		sType, "DrawDistanceAfterTargetsPixels" );
	DRAW_DISTANCE_BEFORE_TARGET_PIXELS.Load(	sType, "DrawDistanceBeforeTargetsPixels" );

	this->SortByDrawOrder();

	m_pPlayerState =		pPlayerState;
	m_pPlayerStageStats =		pPlayerStageStats;
	m_pLifeMeter =			pLM;
	m_pCombinedLifeMeter =		pCombinedLM;
	m_pScoreDisplay =		pScoreDisplay;
	m_pSecondaryScoreDisplay =	pSecondaryScoreDisplay;
	m_pInventory =			pInventory;
	m_pPrimaryScoreKeeper =		pPrimaryScoreKeeper;
	m_pSecondaryScoreKeeper =	pSecondaryScoreKeeper;

	// set initial life
	if( m_pLifeMeter && m_pPlayerStageStats )
	{
		float fLife = m_pLifeMeter->GetLife();
		m_pPlayerStageStats->SetLifeRecordAt( fLife, STATSMAN->m_CurStageStats.m_fStepsSeconds );
	}

	// TODO: Remove use of PlayerNumber.
	PlayerNumber pn = m_pPlayerState->m_PlayerNumber;

	RageSoundLoadParams SoundParams;
	SoundParams.m_bSupportPan = true;
	m_soundMine.Load( THEME->GetPathS(sType,"mine"), true, &SoundParams );

	// calculate M-mod speed here, so we can adjust properly on a per-song basis.
		// XXX: can we find a better location for this?
		// Always calculate the reading bpm, to allow switching to an mmod mid-song.
		/*
		Get Song bpm data
			|---> Is Specified
			|				|---> Not secret (and max specified is different to 0) ---> Returns a value (max 600)
			|				|
			|				|---> Is secret (or max specified is equal to 0) ---> Get Steps Data
			|																			|----> Specified & Not secret &  max specified is different to 0 ---> Returns a value (max 600)
			|																			|
			|																			|----> Else ---> Searchs SONG BPM changes ---> Returns a value (max 600)
			|
			|
			|---> Not specified (uses SONG BPM changes) ---> Returns a value (max 600)

		In the case there's no data from the song bpm (like no #BPMS).. it will crash
		*/

		DisplayBpms bpms;

		// First: check for the Song's specified display bpm, if they are specified
		if( GAMESTATE->IsCourseMode() )
		{
			ASSERT( GAMESTATE->m_pCurTrail[pn] != NULL );
			GAMESTATE->m_pCurTrail[pn]->GetDisplayBpms( bpms );
		}
		else
		{
			ASSERT( GAMESTATE->m_pCurSong != NULL );
			GAMESTATE->m_pCurSong->GetDisplayBpms( bpms );
		}

		float fMaxBPM = 0;

		/* TODO: Find a way to not go above a certain BPM range 
		 * for getting the max BPM. Otherwise, you get songs
		 * like Tsuhsuixamush, M550, 0.18x speed. Even slow
		 * speed readers would not generally find this fun.
		 * -Wolfman2000
		 */
		
		// all BPMs are listed and available, so try them first.
		// get the maximum listed value for the song or course.
		// if the BPMs are < 0, reset and get the actual values.
		if( (GAMESTATE->m_pCurSong->GetDisplayBPM() == DISPLAY_BPM_SPECIFIED) && !bpms.IsSecret() )
		{
			fMaxBPM = (M_MOD_HIGH_CAP > 0 ? 
				   bpms.GetMaxWithin(M_MOD_HIGH_CAP) : 
				   bpms.GetMax());
			fMaxBPM = max( 0, fMaxBPM );
		}

		// we can't rely on the displayed BPMs, so manually calculate.
		if( fMaxBPM == 0 )
		{
			float fThrowAway = 0;

			if( GAMESTATE->IsCourseMode() )
			{
				FOREACH_CONST( TrailEntry, GAMESTATE->m_pCurTrail[pn]->m_vEntries, e )
				{
					float fMaxForEntry;
					if (M_MOD_HIGH_CAP > 0)
						e->pSong->m_SongTiming.GetActualBPM( fThrowAway, fMaxForEntry, M_MOD_HIGH_CAP );
					else 
						e->pSong->m_SongTiming.GetActualBPM( fThrowAway, fMaxForEntry );
					fMaxBPM = max( fMaxForEntry, fMaxBPM );
				}
			}
			else
			{
				DisplayBpms stepsbpms;
				float fStepsMaxBPM = 0;

				// Second: check for the Step's specified display bpm, if they are specified
				if ( GAMESTATE->m_pCurSteps [ pn ]->GetDisplayBPM ( ) == DISPLAY_BPM_SPECIFIED )
				{
					GAMESTATE->m_pCurSteps[pn]->GetDisplayBpms(stepsbpms);
					if ( !stepsbpms.IsSecret ( ) )
					{
						fStepsMaxBPM = (M_MOD_HIGH_CAP > 0 ?
							stepsbpms.GetMaxWithin(M_MOD_HIGH_CAP) :
							stepsbpms.GetMax());
						fStepsMaxBPM = max( 0 , fStepsMaxBPM);
					}
				}

			//GAMESTATE->m_pCurSteps[pn]->GetTimingData()->GetActualBPM( fThrowAway, fMaxBPM, M_MOD_HIGH_CAP );
			// Third: check for the real (actual) bpms in the song
				if ( fStepsMaxBPM == 0 )
				{
					if (M_MOD_HIGH_CAP > 0)
						//GAMESTATE->m_pCurSong->m_SongTiming.GetActualBPM( fThrowAway, fMaxBPM, M_MOD_HIGH_CAP );
						GAMESTATE->m_pCurSong->m_SongTiming.GetActualBPM(fThrowAway, fStepsMaxBPM, M_MOD_HIGH_CAP);
					else
						//GAMESTATE->m_pCurSong->m_SongTiming.GetActualBPM( fThrowAway, fMaxBPM );
						GAMESTATE->m_pCurSong->m_SongTiming.GetActualBPM(fThrowAway, fStepsMaxBPM);
				}

				fMaxBPM = fStepsMaxBPM;
			}
		}

		ASSERT( fMaxBPM > 0 );
		m_pPlayerState->m_fReadBPM = fMaxBPM;

	if( HasVisibleParts() )
	{
		LuaThreadVariable var( "Player", LuaReference::Create(m_pPlayerState->m_PlayerNumber) );
		LuaThreadVariable var2( "MultiPlayer", LuaReference::Create(m_pPlayerState->m_mp) );

		m_pActorWithComboPosition = NULL;
		m_pActorWithJudgmentPosition = NULL;
	}
	else
	{
		m_pActorWithComboPosition = NULL;
		m_pActorWithJudgmentPosition = NULL;
	}

	if( m_pNoteField )
	{
		m_pNoteField->Init( m_pPlayerState, 0 );
		//ActorUtil::LoadAllCommands( *m_pNoteField, sType ); // dont load commands for the notefield
		this->AddChild(m_pNoteField);
	}

	m_fActiveRandomAttackStart = -1.0f;

	// xMAx --------------------------------------------------------------------------------------
	JudgeData JD;

	if ( GAMESTATE->IsBasicMode ( ) )
	{
		JD = SJ; //Added SJ for Basic Mode
	}
	else
	{
		switch ( m_pPlayerState->m_PlayerOptions.GetCurrent ( ).m_iJudgment )
		{
		case 0: JD = EJ; break;
		case 1: JD = NJ; break;
		case 2: JD = HJ; break;
		case 3: JD = VJ; break;
		case 4: JD = XJ; break;
		case 5: JD = UJ; break;
		default: JD = NJ; break;
		}
	}

	float m_fTimingDelay =		JD.iDelay/120.0f;
	float m_fTimingPerfect =	JD.iPerfect/120.0f;
	float m_fTimingDelta =		JD.iDelta/120.0f;

	RoundUpToTwoDecimal( m_fTimingDelay );
	RoundUpToTwoDecimal( m_fTimingPerfect );
	RoundUpToTwoDecimal( m_fTimingDelta );

	PERF_U = -(m_fTimingPerfect + m_fTimingDelay );
	PERF_D = m_fTimingPerfect;

	GREAT_U = PERF_U - m_fTimingDelta;
	GREAT_D = PERF_D + m_fTimingDelta;

	GOOD_U = GREAT_U - m_fTimingDelta;
	GOOD_D = GREAT_D + m_fTimingDelta;

	BAD_U = GOOD_U - m_fTimingDelta;
	BAD_D = GOOD_D + m_fTimingDelta;

	HOLD_TIMING = m_fTimingPerfect + m_fTimingDelay + m_fTimingDelta*3.0f;
	/*
	LOG->Trace( "xMAx::Perfect Timing: %f to %f", PERF_D , PERF_U );
	LOG->Trace( "xMAx::Great Timing: %f to %f", GREAT_D , GREAT_U );
	LOG->Trace( "xMAx::Good Timing: %f to %f", GOOD_D , GOOD_U );
	LOG->Trace( "xMAx::Bad Timing: %f to %f", BAD_D , BAD_U );
	LOG->Trace( "xMAx::Hold Timing: %f", HOLD_TIMING );
	*/
}


/**
 * @brief Determine if a TapNote needs a tap note style judgment.
 * @param tn the TapNote in question.
 * @return true if it does, false otherwise. */
static bool NeedsTapJudging( const TapNote &tn )
{
	// this is only used for "UpdateTapNotesMissedOlderThan"
	if ( tn.judge == TapNote::fake )
		return false;

	switch ( tn.type )
	{
		DEFAULT_FAIL ( tn.type );
		case TapNote::tap:
		case TapNote::hold_head:
		//case TapNote::mine:
		case TapNote::lift:
		case TapNote::hold_tail:
			return tn.result.tns == TNS_None;
		//case TapNote::hold_tail: // added to be judged - xMAx
		case TapNote::mine:	// we dont care if we miss mines - xMAx
		case TapNote::attack:
		case TapNote::autoKeysound:
		//case TapNote::fake:
		case TapNote::empty:
			return false;
	}
}

/**
 * @brief Determine if a TapNote needs a hold note style judgment.
 * @param tn the TapNote in question.
 * @return true if it does, false otherwise. */
static bool NeedsHoldJudging( const TapNote &tn )
{
	if( tn.judge == TapNote::fake )
		return false;

	switch( tn.type )
	{
		DEFAULT_FAIL( tn.type );
		case TapNote::hold_head:
			return tn.HoldResult.hns == HNS_None;
		case TapNote::tap:
		case TapNote::hold_tail:
		case TapNote::mine:
		case TapNote::lift:
		case TapNote::attack:
		case TapNote::autoKeysound:
		//case TapNote::fake:
		case TapNote::empty:
			return false;
	}
}

static void GenerateCacheDataStructure(PlayerState *pPlayerState, const NoteData &notes) {

	pPlayerState->m_CacheDisplayedBeat.clear();

	const vector<TimingSegment*> vScrolls = pPlayerState->GetDisplayedTiming().GetTimingSegments( SEGMENT_SCROLL );

	float displayedBeat = 0.0f;
	float lastRealBeat = 0.0f;
	float lastRatio = 1.0f;
	for ( unsigned i = 0; i < vScrolls.size(); i++ )
	{
		ScrollSegment *seg = ToScroll( vScrolls[i] );
		displayedBeat += ( seg->GetBeat() - lastRealBeat ) * lastRatio;
		lastRealBeat = seg->GetBeat();
		lastRatio = seg->GetRatio();
		CacheDisplayedBeat c = { seg->GetBeat(), displayedBeat, seg->GetRatio() };
		pPlayerState->m_CacheDisplayedBeat.push_back( c );
	}
	
	pPlayerState->m_CacheNoteStat.clear();
	/*
	NoteData::all_tracks_const_iterator it = notes.GetTapNoteRangeAllTracks( 0, MAX_NOTE_ROW, true );
	int count = 0, lastCount = 0;
	for( ; !it.IsAtEnd(); ++it )
	{
		for( int t = 0; t < notes.GetNumTracks(); t++ )
		{
			if( notes.GetTapNote( t, it.Row() ) != TAP_EMPTY ) count ++;
		}
		CacheNoteStat c = { NoteRowToBeat(it.Row()), lastCount, count  };
		lastCount = count;
		pPlayerState->m_CacheNoteStat.push_back(c);
	}
	*/ // xMAx: used in NoteField.cpp
}

void Player::Load()
{
	m_bLoaded = true;

	// TODO: Remove use of PlayerNumber.
	PlayerNumber pn = m_pPlayerState->m_PlayerNumber;

	m_bCountNotesSeparately = m_pPlayerState->m_PlayerOptions.GetCurrent().m_bJudgeByNote || STATSMAN->m_CurStageStats.m_player[pn].m_bStageIsDoublePerformance;

	// Figured this is probably a little expensive so let's cache it
	m_bTickHolds = GAMESTATE->GetCurrentGame()->m_bTickHolds;

	// Remove cached holds and rolls (added for the editor)
	// TODO: Update the hold notes list when start from a different point than the start of the song (for the editor)
	vHoldNotesToUpdate.clear();


	// The editor can start playing in the middle of the song.
	const int iNoteRow = BeatToNoteRowNotRounded(m_pPlayerState->m_Position.m_fSongBeat);
	m_iFirstUncrossedRow = iNoteRow - 1;


	/* The editor reuses Players ... so we really need to make sure everything
	 * is reset and not tweening.  Perhaps ActorFrame should recurse to subactors;
	 * then we could just this->StopTweening()? -glenn */
	 // hurr why don't you just set m_bPropagateCommands on it then -aj
 /*
	 if( m_sprJudgment )
		 m_sprJudgment->PlayCommand("Reset");
 */

 /*
	 if( m_pPlayerStageStats )
	 {
		 SetCombo( m_pPlayerStageStats->m_iCurCombo, m_pPlayerStageStats->m_iCurMissCombo );	// combo can persist between songs and games
	 }

	 if( m_pAttackDisplay )
		 m_pAttackDisplay->Init( m_pPlayerState );
 */ //xMAx- removed

	 /* Don't re-init this; that'll reload graphics.  Add a separate Reset() call
	  * if some ScoreDisplays need it. */
	  //	if( m_pScore )
	  //		m_pScore->Init( pn );

	/* Apply transforms. */
	NoteDataUtil::TransformNoteData( m_NoteData, m_pPlayerState->m_PlayerOptions.GetStage(), GAMESTATE->GetCurrentStyle()->m_StepsType );

	const Song* pSong = GAMESTATE->m_pCurSong;

	m_Timing = GAMESTATE->m_pCurSteps[pn]->GetTimingData();

	// Generate some cache data structure.
	GenerateCacheDataStructure(m_pPlayerState, m_NoteData);

	int iDrawDistanceAfterTargetsPixels = GAMESTATE->IsEditing() ? -100 : DRAW_DISTANCE_AFTER_TARGET_PIXELS;
	int iDrawDistanceBeforeTargetsPixels = GAMESTATE->IsEditing() ? 500 : DRAW_DISTANCE_BEFORE_TARGET_PIXELS;

	if( m_pNoteField && !IsOniDead())
	{
		m_pNoteField->SetY( 70 ); // Original 70 
		m_pNoteField->Load( &m_NoteData, iDrawDistanceAfterTargetsPixels, iDrawDistanceBeforeTargetsPixels, STATSMAN->m_CurStageStats.m_player[pn].m_bStageIsDoublePerformance);

		// xMAx - Estabelece la posicion del NoteField de acuerdo
		bool bUnderAttack = m_pPlayerState->m_PlayerOptions.GetCurrent ().m_fScrolls [ PlayerOptions::SCROLL_UNDER_ATTACK ] > 0.5f; // xMAx
		bool bDrop = m_pPlayerState->m_PlayerOptions.GetCurrent ().m_fScrolls [ PlayerOptions::SCROLL_DROP ] > 0.5f; // xMAx
		bool bNX = m_pPlayerState->m_PlayerOptions.GetCurrent ().m_bNX; // xMAx
		float xRotation = 0;
		float yRotation = 0;

		if( bUnderAttack )
		{
			xRotation += 180;
			yRotation += 180;
			m_pNoteField->SetY ( SCREEN_HEIGHT - 70 );
		}

		if( bDrop )
		{
			xRotation += 180;
			m_pNoteField->SetY ( SCREEN_HEIGHT - 70 );
		}

		if( bNX )
		{
			float xRot = -60;
			//xRotation += -60;
			if( bDrop )
				xRot = 60;
			xRotation += xRot;
			m_pNoteField->SetY ( SCREEN_HEIGHT / 2.0f - 4 );
			//m_pNoteField->SetRotationX(-60);
			m_pNoteField->SetZoom ( 0.635f );
		}

		m_pNoteField->SetBaseRotationY ( yRotation );
		m_pNoteField->SetBaseRotationX ( xRotation );

		if( bUnderAttack && bDrop && !bNX )
			m_pNoteField->SetY ( 70 );
	}
	/*
		bool bPlayerUsingBothSides = GAMESTATE->GetCurrentStyle()->GetUsesCenteredArrows();
		if( m_pAttackDisplay )
			m_pAttackDisplay->SetX( ATTACK_DISPLAY_X.GetValue(pn, bPlayerUsingBothSides) - 40 );
		// set this in Update //m_pAttackDisplay->SetY( bReverse ? ATTACK_DISPLAY_Y_REVERSE : ATTACK_DISPLAY_Y );
	*/ //xMAx

	// set this in Update 
	//m_pJudgment->SetX( JUDGMENT_X.GetValue(pn,bPlayerUsingBothSides) );
	//m_pJudgment->SetY( bReverse ? JUDGMENT_Y_REVERSE : JUDGMENT_Y );

	// Need to set Y positions of all these elements in Update since
	// they change depending on PlayerOptions.


	// Load keysounds.  If sounds are already loaded (as in the editor), don't reload them.
	// XXX: the editor will load several duplicate copies (in each NoteField), and each
	// player will load duplicate sounds.  Does this belong somewhere else (perhaps in
	// a separate object, used alongside ScreenGameplay::m_pSoundMusic and ScreenEdit::m_pSoundMusic?)
	// We don't have to load separate copies to set player fade: always make a copy, and set the
	// fade on the copy.
	RString sSongDir = pSong->GetSongDir();
	m_vKeysounds.resize( pSong->m_vsKeysoundFile.size() );

	// parameters are invalid somehow... -aj
	RageSoundLoadParams SoundParams;
	SoundParams.m_bSupportPan = true;

	float fBalance = GameSoundManager::GetPlayerBalance( pn );
	for( unsigned i=0; i<m_vKeysounds.size(); i++ )
	{
		RString sKeysoundFilePath = sSongDir + pSong->m_vsKeysoundFile[i];
		RageSound& sound = m_vKeysounds[i];
		if( sound.GetLoadedFilePath() != sKeysoundFilePath )
			sound.Load( sKeysoundFilePath, true, &SoundParams );
		sound.SetProperty( "Pan", fBalance );
		sound.SetStopModeFromString( "stop" );
	}

	SAFE_DELETE( m_pIterNeedsTapJudging );
	m_pIterNeedsTapJudging = new NoteData::all_tracks_iterator( m_NoteData.GetTapNoteRangeAllTracks(iNoteRow, MAX_NOTE_ROW) );

	SAFE_DELETE ( m_pIterUncrossedRows );
	m_pIterUncrossedRows = new NoteData::all_tracks_iterator ( m_NoteData.GetTapNoteRangeAllTracks ( iNoteRow, MAX_NOTE_ROW ) );

	SAFE_DELETE( m_pIterNeedsHoldJudging );
	m_pIterNeedsHoldJudging = new NoteData::all_tracks_iterator( m_NoteData.GetTapNoteRangeAllTracks(iNoteRow, MAX_NOTE_ROW ) );
}

void Player::Update( float fDeltaTime )
{
	const RageTimer now;

	// Don't update if we haven't been loaded yet.
	if( !m_bLoaded )
		return;

	if( GAMESTATE->m_pCurSong==NULL || IsOniDead() )
		return;

	ArrowEffects::SetCurrentOptions ( &m_pPlayerState->m_PlayerOptions.GetCurrent () );
	ArrowEffects::Update (); // xMAx - esto estava en NoteField.cpp y ReceptorArrowRow.cpp.. al dope
	ActorFrame::Update( fDeltaTime );

	if(m_pPlayerState->m_mp != MultiPlayer_Invalid)
	{
		/* In multiplayer, it takes too long to run player updates for every player each frame;
		 * with 32 players and three difficulties, we have 96 Players to update.  Stagger these
		 * updates, by only updating a few players each update; since we don't have screen elements
		 * tightly tied to user actions in this mode, this doesn't degrade gameplay.  Run 4 players
		 * per update, which means 12 Players in 3-difficulty mode.
		 */
		static int iCycle = 0;
		iCycle = (iCycle + 1) % 8;

		if((m_pPlayerState->m_mp % 8) != iCycle)
			return;
	}

	// Optimization: Don't spend time processing the things below that won't show 
	// if the Player doesn't show anything on the screen.
	if( HasVisibleParts() )
	{
		// Random Attack Mod
		if( m_pPlayerState->m_PlayerOptions.GetCurrent().m_fRandAttack )
		{
			float fCurrentGameTime = STATSMAN->m_CurStageStats.m_fGameplaySeconds;

			const float fAttackRunTime = ATTACK_RUN_TIME_RANDOM;

			// Don't start until 1 seconds into game, minimum
			if( fCurrentGameTime > 1.0f )
			{
				/* Update the attack if there are no others currently running.
				 * Note that we have a new one activate a little early; This is
				 * to have a bit of overlap rather than an abrupt change. */
				if( (fCurrentGameTime - m_fActiveRandomAttackStart) > (fAttackRunTime - 0.5f) )
				{
					m_fActiveRandomAttackStart = fCurrentGameTime;

					Attack attRandomAttack;
					attRandomAttack.sModifiers = ApplyRandomAttack();
					attRandomAttack.fSecsRemaining = fAttackRunTime;
					m_pPlayerState->LaunchAttack( attRandomAttack );
				}
			}
		}

		float fMiniPercent = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fEffects[PlayerOptions::EFFECT_MINI];
		float fTinyPercent = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fEffects[PlayerOptions::EFFECT_TINY];
		float fJudgmentZoom = min( powf(0.5f, fMiniPercent+fTinyPercent), 1.0f );

		// Update Y positions
		/*
		{
			for( int c=0; c<GAMESTATE->GetCurrentStyle()->m_iColsPerPlayer; c++ )
			{
				float fPercentReverse = m_pPlayerState->m_PlayerOptions.GetCurrent().GetReversePercentForColumn(c);
				float fHoldJudgeYPos = SCALE( fPercentReverse, 0.f, 1.f, HOLD_JUDGMENT_Y_STANDARD, HOLD_JUDGMENT_Y_REVERSE );
				//float fGrayYPos = SCALE( fPercentReverse, 0.f, 1.f, GRAY_ARROWS_Y_STANDARD, GRAY_ARROWS_Y_REVERSE );

				float fX = ArrowEffects::GetXPos( m_pPlayerState, c, 0 );
				const float fZ = ArrowEffects::GetZPos( c, 0 );
				fX *= ( 1 - fMiniPercent * 0.5f );

				m_vpHoldJudgment[c]->SetX( fX );
				m_vpHoldJudgment[c]->SetY( fHoldJudgeYPos );
				m_vpHoldJudgment[c]->SetZ( fZ );
				m_vpHoldJudgment[c]->SetZoom( fJudgmentZoom );
			}
		}
		*/ //xMAx

		// NoteField accounts for reverse on its own now.
		//if( m_pNoteField )
		//	m_pNoteField->SetY( fGrayYPos );

		const bool bReverse = m_pPlayerState->m_PlayerOptions.GetCurrent().GetReversePercentForColumn(0) == 1;
		float fPercentCentered = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fScrolls[PlayerOptions::SCROLL_CENTERED];

		if( m_pActorWithJudgmentPosition != NULL )
		{
			const Actor::TweenState &ts1 = m_tsJudgment[bReverse?1:0][0];
			const Actor::TweenState &ts2 = m_tsJudgment[bReverse?1:0][1];
			Actor::TweenState::MakeWeightedAverage( m_pActorWithJudgmentPosition->DestTweenState(), ts1, ts2, fPercentCentered );
		}

		if( m_pActorWithComboPosition != NULL )
		{
			const Actor::TweenState &ts1 = m_tsCombo[bReverse?1:0][0];
			const Actor::TweenState &ts2 = m_tsCombo[bReverse?1:0][1];
			Actor::TweenState::MakeWeightedAverage( m_pActorWithComboPosition->DestTweenState(), ts1, ts2, fPercentCentered );
		}

		float fNoteFieldZoom = 1 - fMiniPercent*0.5f;
		if( m_pNoteField )
			m_pNoteField->SetZoom( fNoteFieldZoom );
		if( m_pActorWithJudgmentPosition != NULL )
			m_pActorWithJudgmentPosition->SetZoom( m_pActorWithJudgmentPosition->GetZoom() * fJudgmentZoom );
		if( m_pActorWithComboPosition != NULL )
			m_pActorWithComboPosition->SetZoom( m_pActorWithComboPosition->GetZoom() * fJudgmentZoom );
	}

	// If we're paused, don't update tap or hold note logic, so hold notes can be released
	// during pause.
	if( m_bPaused )
		return;

	// update pressed flag
	const int iNumCols = GAMESTATE->GetCurrentStyle()->m_iColsPerPlayer;
	ASSERT_M( iNumCols <= MAX_COLS_PER_PLAYER, ssprintf("%i > %i", iNumCols, MAX_COLS_PER_PLAYER) );
	for( int col=0; col < iNumCols; ++col )
	{
		ASSERT( m_pPlayerState != NULL );

		// TODO: Remove use of PlayerNumber.
		GameInput GameI = GAMESTATE->GetCurrentStyle()->StyleInputToGameInput( col, m_pPlayerState->m_PlayerNumber );

		bool bIsHoldingButton = INPUTMAPPER->IsBeingPressed( GameI );

		// TODO: Make this work for non-human-controlled players
		if( bIsHoldingButton && !GAMESTATE->m_bDemonstrationOrJukebox && m_pPlayerState->m_PlayerController==PC_HUMAN )
			if( m_pNoteField )
				m_pNoteField->SetPressed( col );
	}

	const int iRowNowRounded = BeatToNoteRow ( m_pPlayerState->m_Position.m_fSongBeat );
	CrossedHoldsRows ( iRowNowRounded, now, fDeltaTime );

	
	// Why was this originally "BeatToNoteRowNotRounded"? It should be rounded. -Chris
	/* We want to send the crossed row message exactly when we cross the row--not
	* .5 before the row. Use a very slow song (around 2 BPM) as a test case: without
	* rounding, autoplay steps early. -glenn */
	const int iRowNow = BeatToNoteRowNotRounded( m_pPlayerState->m_Position.m_fSongBeat );
	if( iRowNow >= 0 )
	{
		if( GAMESTATE->IsPlayerEnabled(m_pPlayerState) )
		{
			if(m_pPlayerState->m_Position.m_bDelay)
			{
				if( !m_bDelay )
					m_bDelay = true;
			}
			else
			{
				if(m_bDelay)
				{
					if(m_pPlayerState->m_PlayerController != PC_HUMAN)
					{
						CrossedRows( iRowNow-1, now );
					}
					m_bDelay = false;
				}
				CrossedRows( iRowNow, now );
			}
		}
	}
	
	// Check for completely judged rows.
	//UpdateJudgedRows();

	// Check for TapNote misses
	if (!GAMESTATE->m_bInStepEditor)
	{
		UpdateTapNotesMissedOlderThan( GetMaxStepDistanceSeconds() );
	}

	// process transforms that are waiting to be applied
	ApplyWaitingTransforms();
}

void Player::UpdateHoldNote( int iSongRow, float fDeltaTime, TrackRowTapNote &trtn )
{
	// Get the tap note in "tn" to work more comfortable
	TapNote &tn = *( trtn.pTN );

	ASSERT_M ( tn.type == TapNote::hold_head, "ASSERT_P1" );

	int iStartRow =		trtn.iRow;
	int iMaxEndRow =	iStartRow + tn.iDuration;
	int iTrack =		trtn.iTrack;
	TapNote::SubType subType = tn.subType;

	// Set hold flags so NoteField can do intelligent drawing
	tn.HoldResult.bHeld =	false;
	tn.HoldResult.bActive = false;

	// Check first if the head is inside a fake or warp area and judge it so checkpoints can miss
	/*if( !this->m_Timing->IsJudgableAtRow(iSongRow) )
		return;*/

	// Get the hold info
	HoldNoteScore hns =	tn.HoldResult.hns;
	float fLife =		tn.HoldResult.fLife;

	// If the hold was judged, go out
	if( hns != HNS_None )
	{
		return;
	}

	// Update fOverLappedTime
	if( iStartRow <= iSongRow && iSongRow <= iMaxEndRow )
	{
		tn.HoldResult.fOverlappedTime += fDeltaTime;
	}
	else
	{
		tn.HoldResult.fOverlappedTime = 0;
	}


	bool bIsHoldingButton = false;
	bool bHeadJudged = tn.result.tns != TNS_None;

	//bool bHoldTailIsJudgable = this->m_Timing->IsJudgableAtRow(iMaxEndRow);
	if( m_pPlayerState->m_PlayerController != PC_HUMAN )
	{
		bIsHoldingButton = true;

		if( !bHeadJudged && iStartRow <= iSongRow )
		{
			if( this->m_Timing->IsJudgableAtRow ( iStartRow ) )
			{
				const RageTimer now;
				Step ( iTrack, iStartRow, now, false );
				bHeadJudged = true;
			}
			else
			{
				// hold head cant be judged. Enable life count for the holds parts that are out of the fake area (gibing a tns score)
				tn.result.tns = TNS_W2;
				bHeadJudged = true;
			}
		}

		STATSMAN->m_CurStageStats.m_bUsedAutoplay = true;
		if( m_pPlayerStageStats != NULL )
		{
			m_pPlayerStageStats->m_bDisqualified = true;
		}
	}
	else
	{
		PlayerNumber pn = m_pPlayerState->m_PlayerNumber;
		GameInput GameI = GAMESTATE->GetCurrentStyle ()->StyleInputToGameInput ( iTrack, pn );

		bIsHoldingButton |= INPUTMAPPER->IsBeingPressed(GameI, m_pPlayerState->m_mp);

		// m_FreePerformance
		if( GAMESTATE->m_pPlayerState [ pn ]->m_PlayerOptions.GetCurrent ().m_bFreePerformance && ( GAMESTATE->GetCurrentStyle ()->m_iColsPerPlayer > 5 ) )
		{
			if( GAMESTATE->GetCurrentStyle ()->m_iColsPerPlayer == 10 ) //piu double
			{
				if( iTrack > 4 ) // player 2 side
				{
					GameInput GameI_tmp = GAMESTATE->GetCurrentStyle ()->StyleInputToGameInput ( iTrack - 5, pn );
					bIsHoldingButton |= INPUTMAPPER->IsBeingPressed ( GameI_tmp, m_pPlayerState->m_mp );
				}
				else // player 1 side
				{
					GameInput GameI_tmp = GAMESTATE->GetCurrentStyle ()->StyleInputToGameInput ( iTrack + 5, pn );
					bIsHoldingButton |= INPUTMAPPER->IsBeingPressed ( GameI_tmp, m_pPlayerState->m_mp );
				}
			}
			else if( GAMESTATE->GetCurrentStyle ()->m_iColsPerPlayer == 6 ) // piu halfdouble
			{
				if( iTrack == 0) // Only the center is available
				{
					GameInput GameI_tmp = GAMESTATE->GetCurrentStyle()->StyleInputToGameInput ( 5, pn );
					bIsHoldingButton |= INPUTMAPPER->IsBeingPressed ( GameI_tmp, m_pPlayerState->m_mp );	
				}
				else if( iTrack == 5 ) // Only the center is available
				{
					GameInput GameI_tmp = GAMESTATE->GetCurrentStyle()->StyleInputToGameInput ( 0, pn );
					bIsHoldingButton |= INPUTMAPPER->IsBeingPressed ( GameI_tmp, m_pPlayerState->m_mp );
				}
			}
		}

		if( !bHeadJudged && iStartRow <= iSongRow )
		{
			if( this->m_Timing->IsJudgableAtRow ( iStartRow ) )
			{
				if( bIsHoldingButton )
				{
					const RageTimer now;
					Step ( iTrack, iStartRow, now, false );
					bHeadJudged = true;
				}
			}
			else
			{
				// hold head cant be judged. Enable life and checkpoints count for the holds parts that are out of the fake area (giving a tns score)
				tn.result.tns = TNS_W2;
				bHeadJudged = true;
			}
		}
	}

	//If this row can't be judged then dont continue
	if( !this->m_Timing->IsJudgableAtRow ( iSongRow ) )
	{
		if( iSongRow >= iMaxEndRow )
		{
			//Use Missed here. If we use Held the rest of hold will disappear
			//Make this so the game can delete this hold from the holds list
			tn.HoldResult.hns = HNS_Missed;
		}
		return;
	}

	// If head was judged then continue with the life checking
	if( bHeadJudged || tn.subType == TapNote::hold_head_roll )
	{
		//If the song beat is in the range of this hold:
		if( iStartRow <= iSongRow )
		{
			switch( subType )
			{
				case TapNote::hold_head_hold:
				{
					//set hold flag so NoteField can do intelligent drawing
					tn.HoldResult.bHeld = bIsHoldingButton;
					tn.HoldResult.bActive = true; // bInitiatedNote

					if( bIsHoldingButton ) // bInitiatedNote && bIsHoldingButton
					{
						// Increase life
						fLife = MAX_HOLD_LIFE;
					}
					else
					{
						// Decrease life
						fLife -= ( fDeltaTime / HOLD_TIMING ) * GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate; // xMAx
						fLife = max ( fLife, 0 );
					}
				};
				break;
				case TapNote::hold_head_roll:
				{
					tn.HoldResult.bHeld = true;
					tn.HoldResult.bActive = true; // bInitiatedNote

					// give positive life in Step(), not here.

					// Decrease life
					//fLife -= (fDeltaTime/GetWindowSeconds(TW_Roll))*GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate; // xMAx
					fLife -= ( fDeltaTime / HOLD_TIMING ) * GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate; // xMAx
					fLife = ( fLife, 0 );
				};
				break;
				default:
					FAIL_M( ssprintf( "Invalid tap note subtype: %i", subType ) );
			}
		}

		//Update  the last held row (if life is positive)
		if( fLife > 0 )
		// if( fLife != 0 )
		{
			tn.HoldResult.iLastHeldRow = min( iSongRow, iMaxEndRow );
		}

		// Get the HoldNoteJudge (or score) if the hold have passed the current song row
		if( iSongRow >= iMaxEndRow )
		{
			if( fLife > 0 )
			{
				hns = HNS_Held;

				// Judge the tail, so the taps in the same row can know that it has been judged. Only for normal holds
				/*if( tn.subType == TapNote::hold_head_hold && this->m_Timing->IsJudgableAtRow(iMaxEndRow) )
				{
					const RageTimer now;
					Step( iTrack, iMaxEndRow, now, false );
				}*/
			}
			else
			{
				hns = HNS_Missed;
			}
		}
	}

	tn.HoldResult.fLife = fLife;
	tn.HoldResult.hns = hns;

	// Update rolls autoplay (each time they got under 0.5f of life)
	if( tn.subType == TapNote::hold_head_roll && m_pPlayerState->m_PlayerController != PC_HUMAN && tn.HoldResult.fLife < 0.5f )
	{
		const RageTimer now;
		Step( iTrack, -1, now, false ); // Use row = -1, so Step() can check for rolls

		STATSMAN->m_CurStageStats.m_bUsedAutoplay = true;
		if( m_pPlayerStageStats != NULL )
			m_pPlayerStageStats->m_bDisqualified = true;
	}
}

void Player::ApplyWaitingTransforms()
{
	if( m_pPlayerState->m_ModsToApply.size() == 0 )
		return;

	for( unsigned j=0; j<m_pPlayerState->m_ModsToApply.size(); j++ )
	{
		const Attack &mod = m_pPlayerState->m_ModsToApply[j];
		PlayerOptions po;
		// if re-adding noteskin changes, blank out po.m_sNoteSkin. -aj
		po.FromString( mod.sModifiers );

		float fStartBeat, fEndBeat;
		mod.GetRealtimeAttackBeats( GAMESTATE->m_pCurSong, m_pPlayerState, fStartBeat, fEndBeat );
		fEndBeat = min( fEndBeat, m_NoteData.GetLastBeat() );

		//LOG->Trace( "Applying transform '%s' from %f to %f to '%s'", mod.sModifiers.c_str(), fStartBeat, fEndBeat,
		//	GAMESTATE->m_pCurSong->GetTranslitMainTitle().c_str() );

		// if re-adding noteskin changes, this is one place to edit -aj
		NoteDataUtil::TransformNoteData( m_NoteData, po, GAMESTATE->GetCurrentStyle()->m_StepsType, BeatToNoteRow(fStartBeat), BeatToNoteRow(fEndBeat) );
	}
	m_pPlayerState->m_ModsToApply.clear();
}

void Player::DrawPrimitives()
{
	// TODO: Remove use of PlayerNumber.
	PlayerNumber pn = m_pPlayerState->m_PlayerNumber;

	// May have both players in doubles (for battle play); only draw primary player.
	if( GAMESTATE->GetCurrentStyle()->m_StyleType == StyleType_OnePlayerTwoSides  &&  pn != GAMESTATE->GetMasterPlayerNumber() )
		return;

	if (m_pNoteField) // && !IsOniDead()) // xMAx - removed
	{
		DISPLAY->CameraPushMatrix();
		DISPLAY->PushMatrix();

		if (m_pPlayerState->m_PlayerOptions.GetCurrent().m_bNX)
		{
			DISPLAY->LoadMenuPerspective(90, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH / 2, (SCREEN_HEIGHT / 2));

			/*
			m_pNoteField->SetY(SCREEN_HEIGHT/2.0f - 4);
			m_pNoteField->SetRotationX(-60);
			m_pNoteField->SetZoom(0.635f);
			*/

			//m_pNoteField->Draw();
		}
		else
		{
			float fTilt = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fPerspectiveTilt;
			float fSkew = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fSkew;

			//xMAx
			float	fMini = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fEffects[PlayerOptions::EFFECT_MINI];
			bool	bBumpy = m_pPlayerState->m_PlayerOptions.GetCurrent().m_fEffects[PlayerOptions::EFFECT_BUMPY] != 0;

			//float fCenterY = this->GetY()+(GRAY_ARROWS_Y_STANDARD+GRAY_ARROWS_Y_REVERSE)/2; //xMAx - default
			//DISPLAY->LoadMenuPerspective( 100, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_CENTER_X, fCenterY ); //ESTO ES SOLO PARA NX MOD

			float	fReverse = m_pPlayerState->m_PlayerOptions.GetCurrent().GetReversePercentForColumn(0);
			bool	bReverse = fReverse != 0;
			float	fRotX = 0;

			//xMAx - Condiciones agregadas para no cargar un LoadMenuPerspective cuando no es necesario.
			if (bBumpy || fTilt != 0 || fSkew != 0 || bReverse)
				DISPLAY->LoadMenuPerspective(45, SCREEN_WIDTH, SCREEN_HEIGHT, SCALE(fSkew, 0.f, 1.f, this->GetX(), SCREEN_CENTER_X), (SCREEN_HEIGHT / 2));

			if (bReverse)
			{
				//m_pNoteField->SetRotationX( 180*fReverse );	//ICTB
				fRotX = 180 * fReverse;
				//m_pNoteField->SetY( 70 + (SCREEN_HEIGHT-140)*fReverse); 
				this->SetY((SCREEN_HEIGHT)*fReverse);
			}

			if (fTilt != 0)
			{
				float fTiltDegrees = SCALE(fTilt, -1.f, +1.f, +30, -30) * (bReverse ? -1 : 1);
				fRotX += fTiltDegrees;
				//m_pNoteField->SetRotationX( fTiltDegrees );	//ICTB
			}

			//m_pNoteField->SetRotationX( fRotX );
			this->SetRotationX(fRotX);

			if (fMini != 0)
			{
				float fZoom = SCALE(fMini, 0.f, 1.f, 1.f, 0.5f);
				m_pNoteField->SetZoom(fZoom);
			}

			//m_pNoteField->Draw();
		}

		m_pNoteField->Draw();
		DISPLAY->CameraPopMatrix();
		DISPLAY->PopMatrix();
	}
}

void Player::ChangeLife( TapNoteScore tns )
{
	PlayerNumber pn = m_pPlayerState->m_PlayerNumber;
	if( m_pLifeMeter )
		m_pLifeMeter->ChangeLife( tns );

	if( m_pCombinedLifeMeter )
		m_pCombinedLifeMeter->ChangeLife( pn, tns );

	//ChangeLifeRecord(); //this's to record the gameplay.. no need in SF2

	switch( tns )
	{
	case TNS_None:
	case TNS_Miss:
	case TNS_CheckpointMiss:
	case TNS_HitMine:
		++m_pPlayerState->m_iTapsMissedSinceLastHasteUpdate;
		break;
	default:
		++m_pPlayerState->m_iTapsHitSinceLastHasteUpdate;
		break;
	}
}

bool Player::IsOniDead() const
{
	// If we're playing on oni and we've died, do nothing.
	return GAMESTATE->m_SongOptions.GetCurrent().m_LifeType == LifeType_Battery && m_pPlayerStageStats && m_pPlayerStageStats->m_bFailed;
}

void Player::PlayKeysound( const TapNote &tn, TapNoteScore score )
{
	// tap note must have keysound
	if( tn.iKeysoundIndex >= 0 && tn.iKeysoundIndex < ( int ) m_vKeysounds.size() )
	{
		// handle a case for hold notes
		if( tn.type == TapNote::hold_head )
		{
			// if the hold is not already held
			if( tn.HoldResult.hns == HNS_None )
			{
				// if the hold is already activated
				TapNoteScore tns = tn.result.tns;
				if( tns != TNS_None && tns != TNS_Miss && score == TNS_None )
				{
					// the sound must also be already playing
					if( m_vKeysounds [ tn.iKeysoundIndex ].IsPlaying() )
					{
						// if all of these conditions are met, don't play the sound.
						return;
					}
				}
			}
		}
		m_vKeysounds [ tn.iKeysoundIndex ].Play();
		Preference<float> *pVolume = Preference<float>::GetPreferenceByName( "SoundVolume" );
		float fVol = pVolume->Get();
		m_vKeysounds [ tn.iKeysoundIndex ].SetProperty( "Volume", fVol );
	}
}

int Player::GetClosestNoteDirectional( int col, int iStartRow, int iEndRow, bool bAllowGraded, bool bForward, bool bAllowHoldHead ) const
{
	NoteData::const_iterator nbegin, nend, begin, end;

	m_NoteData.GetTapNoteRange( col, iStartRow, iEndRow, nbegin, nend );

	begin = nbegin;
	end = nend;

	if( !bForward )
	{
		swap( begin, end );
	}

	bool bFirstCheck = true;
	for( ; begin != end; bForward ? ++begin : --begin )
	{
		// xMAx: to see why we need this, check the definition of "GetTapNoteRange", it uses "lower_bound" for both limits
		if( bFirstCheck && !bForward )
		{
			--begin;
			--end;
			bFirstCheck = false;
		}

		const TapNote &tn = begin->second;
		const int iRow = begin->first;

		if( tn.judge == TapNote::fake || tn.type == TapNote::empty || tn.type == TapNote::autoKeysound || tn.type == TapNote::hold_tail )
			continue;

		if( tn.judge == TapNote::hold_head && !bAllowHoldHead )
			continue;

		if( tn.result.tns != TNS_None && !bAllowGraded )
			continue;

		if( !m_Timing->IsJudgableAtRow( iRow ) )
			continue;

		return iRow;
	}

	return -1;
}

int Player::GetClosestNote( int col, int iNoteRow, int iMaxRowsAhead, int iMaxRowsBehind, bool bAllowGraded, bool bAllowHoldHead ) const
{
	// Start at iIndexStartLookingAt and search outward.
	int iNI = GetClosestNoteDirectional( col, iNoteRow, iNoteRow+iMaxRowsAhead, bAllowGraded, true, bAllowHoldHead );
	int iPI = GetClosestNoteDirectional( col, iNoteRow-iMaxRowsBehind, iNoteRow, bAllowGraded, false, bAllowHoldHead );

	if( iNI == -1 && iPI == -1 ) return -1;
	if( iNI == -1 ) return iPI;
	if( iPI == -1 ) return iNI;

	// Get the current time, previous time, and next time.
	float fNoteTime = m_pPlayerState->m_Position.m_fMusicSeconds	;
	float fNextTime = m_Timing->GetElapsedTimeFromBeat(NoteRowToBeat(iNI));
	float fPrevTime = m_Timing->GetElapsedTimeFromBeat(NoteRowToBeat(iPI));

	/* Figure out which row is closer. */
	if( fabsf(fNoteTime-fNextTime) > fabsf(fNoteTime-fPrevTime) )
		return iPI;
	else
		return iNI;
}

void Player::Step ( int col, int row, const RageTimer &tm, bool bRelease )
{
	if( IsOniDead() )
		return;

	int iSongRow = -1;

	float fPositionSeconds	=	m_pPlayerState->m_Position.m_fMusicSeconds - tm.Ago();
	const float fLastBeatUpdate =	m_pPlayerState->m_Position.m_LastBeatUpdate.Ago();
	const float fTimeSinceStep =	tm.Ago();


	// xMAx: if row == -1, then Step was called from the Gameplay.. else, it was called from a function here
	if( row == -1 )
	{
		float fSongBeat = m_Timing->GetBeatFromElapsedTime( fPositionSeconds );
		iSongRow = BeatToNoteRow( fSongBeat );
	}
	else
	{
		iSongRow = row;
	}

	// Update steps count
	if( m_pPlayerStageStats && !bRelease && row == -1 )
	{
		m_pPlayerStageStats->m_iNumControllerSteps++;
	}

	// Update holds life/rolls checkpoints
	bool bFoundRoll = false;
	if( vHoldNotesToUpdate.size() > 0 && col != -1 && row == -1 && !bRelease ) //Check for row == -1 so we only check when Step() is called by ScreenGameplay or autoplay
	{
		// Check for a Roll in the current song row
		int iHeadRow;
		if( m_NoteData.IsHoldNoteAtRow( col, iSongRow, &iHeadRow ) )
		{
			TapNote *pTN = NULL;
			NoteData::iterator iter = m_NoteData.FindTapNote( col, iHeadRow );
			DEBUG_ASSERT( iter != m_NoteData.end( col ) );
			pTN = &iter->second;

			if( pTN->type == TapNote::hold_head && pTN->subType == TapNote::hold_head_roll && pTN->HoldResult.hns == HNS_None && pTN->result.tns != TNS_None )
			{
				const int iEndRow = iHeadRow + pTN->iDuration;

				//If the song beat is in the range of this hold:
				if( iHeadRow <= iSongRow && iSongRow <= iEndRow )
				{
					bFoundRoll = true;

					// Increase life
					pTN->HoldResult.fLife = 1;
					pTN->HoldResult.iLastHeldRow = min( iSongRow, iEndRow );

					vector<int> viColsWithHold;
					viColsWithHold.push_back( col );

					// xMAx - added to increase combo and perfect count
					HandleHoldCheckpoint( iHeadRow, 1, 0, viColsWithHold, true );// xMNAx - bHoldsAreBeingPressed
				}
			}
		}
	}

	if( bFoundRoll )
		return;

	//const float fScrollArRow = m_Timing->GetScrollAtRow( iSongRow ); //usado para hacer una prueba -xMAx
	//const float fSpeedArRow = 1.0f;//m_Timing->GetSpeedPercentAtRow( iSongRow ); //usado para hacer una prueba -xMAx
	//int factor = (int)((1.0f/(fScrollArRow==0.0f?0.0001f:fScrollArRow))*(1.0f/(fSpeedArRow==0.0f?0.0001f:fSpeedArRow)));
	// calculate TapNoteScore
	TapNoteScore score = TNS_None;
	int iRowOfOverlappingNoteOrRow = -1;

	if( row != -1 )
	{
		score = AllowW1() ? TNS_W1 : TNS_W2;
		iRowOfOverlappingNoteOrRow = row;
	}
	else
	{
		//const int iMaxRowsAhead = 	BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds + abs(BAD_D)*factor ) ) + ROWS_PER_BEAT;
		const int iMaxRowsAhead = BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds + abs( BAD_D ) ) ) + ROWS_PER_BEAT;
		//const int iMaxRowsBehind =  BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds - abs(BAD_U)*factor ) ) - ROWS_PER_BEAT;
		const int iMaxRowsBehind = BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds + abs( BAD_D ) ) ) + ROWS_PER_BEAT;

		iRowOfOverlappingNoteOrRow = GetClosestNote( col, iSongRow, iMaxRowsAhead, iMaxRowsBehind, false, true );
	}

	if( iRowOfOverlappingNoteOrRow != -1 )
	{
		// compute the score for this hit
		const float fStepBeat = NoteRowToBeat( ( float ) iRowOfOverlappingNoteOrRow );
		const float fStepSeconds = m_Timing->GetElapsedTimeFromBeat( fStepBeat );

		/* GAMESTATE->m_fMusicSeconds is the music time as of GAMESTATE->m_LastBeatUpdate. Figure
		* out what the music time is as of now. */

		//const float fCurrentMusicSeconds = m_pPlayerState->m_Position.m_fMusicSeconds + (fLastBeatUpdate*GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate);
		const float fCurrentMusicSeconds = m_pPlayerState->m_Position.m_fMusicSeconds + fLastBeatUpdate;

		// ... which means it happened at this point in the music:
		//const float fMusicSeconds = fCurrentMusicSeconds - fTimeSinceStep * GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate;
		const float fMusicSeconds = fCurrentMusicSeconds - fTimeSinceStep;

		// The offset from the actual step in seconds:
		//fNoteOffset = (fStepSeconds - fMusicSeconds) / GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate;	// account for music rate
		//const float fNoteOffset = ((int((fStepSeconds - fMusicSeconds)*1000))/1000.0f)*fScrollArRow*fSpeedArRow;  //usado para hacer una prueba -xMAx
		const float fNoteOffset = ( ( int( ( fStepSeconds - fMusicSeconds ) * 1000 ) ) / 1000.0f ); // fStepSeconds - fMusicSeconds;

		//LOG->Trace( "xMAx::Original Noteoffset is %f at row %d", (fStepSeconds - fMusicSeconds), iRowOfOverlappingNoteOrRow);
		//LOG->Trace( "xMAx::Fixed Noteoffset is %f at row %d", fNoteOffset, iRowOfOverlappingNoteOrRow);
		const float fSecondsFromExact = fabsf( fNoteOffset );

		TapNote *pTN = NULL;
		NoteData::iterator iter = m_NoteData.FindTapNote( col, iRowOfOverlappingNoteOrRow );
		DEBUG_ASSERT( iter != m_NoteData.end( col ) );
		pTN = &iter->second;

		switch( m_pPlayerState->m_PlayerController )
		{
			case PC_HUMAN:
				switch( pTN->type )
				{
					case TapNote::mine:
						//Stepped too close to mine?
						if( !bRelease && fSecondsFromExact <= GetWindowSeconds( TW_Mine ) )
						{
							score = TNS_HitMine;

							if( m_pPlayerState->m_PlayerNumber == PLAYER_1 )
								SCREENMAN->PostMessageToTopScreen( SM_Player1HitMine, 0 );
							else
								SCREENMAN->PostMessageToTopScreen( SM_Player2HitMine, 0 );

							if( m_pNoteField )
								m_pNoteField->DidTapNote( col, score, false );

							/* Attack Mines:
							 * Only difference is these launch an attack rather than affecting
							 * the lifebar. All the other mine impacts (score, dance points,
							 * etc.) are still applied. */
							if( m_pPlayerState->m_PlayerOptions.GetCurrent().m_bTransforms [ PlayerOptions::TRANSFORM_ATTACKMINES ] )
							{
								const float fAttackRunTime = ATTACK_RUN_TIME_MINE;

								Attack attMineAttack;
								attMineAttack.sModifiers = ApplyRandomAttack();
								attMineAttack.fStartSecond = ATTACK_STARTS_NOW;
								attMineAttack.fSecsRemaining = fAttackRunTime;

								m_pPlayerState->LaunchAttack( attMineAttack );
							}
							else
								ChangeLife( TNS_HitMine );
							/*
							if( m_pPrimaryScoreKeeper )
							m_pPrimaryScoreKeeper->HandleTapScore( *pTN );
							*/ // check below
							HideNote( col, iRowOfOverlappingNoteOrRow );

							if( m_soundMine.IsPlaying() )
								m_soundMine.Stop();

							m_soundMine.Play();
						};
						break;
					case TapNote::attack:
						if( !bRelease && fSecondsFromExact <= GetWindowSeconds( TW_Attack ) && !pTN->result.bHidden )
							score = AllowW1() ? TNS_W1 : TNS_W2;
						break;
					case TapNote::hold_tail:
						if( BAD_U <= fNoteOffset && fNoteOffset <= PERF_D )
						{
							score = TNS_W2;
							pTN->HoldResult.iCheckpointsHit++;
						};
						break;
					case TapNote::hold_head:
						if( ( BAD_U <= fNoteOffset && fNoteOffset <= PERF_D ) ) // || row != -1
						{
							score = TNS_W2;
							pTN->HoldResult.iCheckpointsHit++; // xMAx: (added) the hold head is counted as a checkpoint

							// Searchs for taps over the hold that were not judged, only when the player actually step the hold (not when its being held)- xMAx
							// 0.208333 is BAD_U in normal judgment - xMAx
							const int iMaxRowBehind = BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds - 0.208333f ) ) - ROWS_PER_BEAT;

							// Search taps before hold heads ONLY when this function is called by ScreenGameplay, not when's called by UpdateHoldNote() function
							if( row == -1 )
							{
								int iRowOfTapInsideHoldWindow = GetClosestNote( col, iSongRow, iSongRow, iMaxRowBehind, false, false );
								if( iRowOfTapInsideHoldWindow != -1 && ( iRowOfTapInsideHoldWindow < iRowOfOverlappingNoteOrRow ) )
								{
									TapNote *pTNN = NULL;
									NoteData::iterator iter = m_NoteData.FindTapNote( col, iRowOfTapInsideHoldWindow );
									DEBUG_ASSERT( iter != m_NoteData.end( col ) );
									pTNN = &iter->second;

									pTNN->result.tns = score;
									pTNN->result.fTapNoteOffset = 0;
									HideNote( col, iRowOfTapInsideHoldWindow );
								}
							}

							// Rescue checkpoint
							if( pTN->subType == TapNote::hold_head_hold && pTN->HoldResult.viCheckpointsNotJudged.size() > 0 )
							{
								int cp = pTN->HoldResult.viCheckpointsNotJudged.size();
								pTN->HoldResult.iCheckpointsHit += cp;

								for( int i = 0; i < cp; i++ )
								{
									HandleTapRowScore( pTN->HoldResult.viCheckpointsNotJudged [ i ], TNS_CheckpointHit );
								}

								pTN->HoldResult.viCheckpointsNotJudged.clear();
							};

							// Send messages in Editor only for hold heads - xMAx
							if( GAMESTATE->IsEditing() )
							{
								Message msg( "CheckpointsPerfectCount" );
								msg.SetParam( "cp", pTN->HoldResult.iCheckpointsHit );
								MESSAGEMAN->Broadcast( msg );
							}
						};
						break;
					default:
						if( ( pTN->type == TapNote::lift ) == bRelease )
						{
							if( fNoteOffset < 0 )
							{
								if( PERF_U <= fNoteOffset ) score = TNS_W2;
								else
								{
									if( GREAT_U <= fNoteOffset ) score = TNS_W3;
									else
									{
										if( GOOD_U <= fNoteOffset ) score = TNS_W4;
										else
										{
											if( BAD_U <= fNoteOffset ) score = TNS_W5;
										}
									}
								}
							}
							else
							{
								if( fNoteOffset <= PERF_D ) score = TNS_W2;
								else
								{
									if( fNoteOffset <= GREAT_D ) score = TNS_W3;
									else
									{
										if( fNoteOffset <= GOOD_D ) score = TNS_W4;
										else
										{
											if( fNoteOffset <= BAD_D ) score = TNS_W5;
										}
									}
								}
							}
						}
						break;
				};
				break;
				case PC_CPU:
				case PC_AUTOPLAY:
					switch( pTN->type )
					{
						case TapNote::mine:
							score = TNS_AvoidMine;
							break;
						case TapNote::hold_head:
							pTN->HoldResult.iCheckpointsHit++; // xMAx: (added) the hold head is counted as a checkpoint

							// Send messages in Editor only for hold heads - xMAx
							if( GAMESTATE->IsEditing() )
							{
								Message msg( "CheckpointsPerfectCount" );
								msg.SetParam( "cp", pTN->HoldResult.iCheckpointsHit );
								MESSAGEMAN->Broadcast( msg );
							}
						default:
							score = TNS_W2;
							break;
					};
					break;
				default:
					FAIL_M( ssprintf( "Invalid player copntroller type: %i", m_pPlayerState->m_PlayerController ) );
		}

		// Do game-specific and mode-specific score mapping.
		score = GAMESTATE->GetCurrentGame()->MapTapNoteScore( score );
		if( score == TNS_W1 && !GAMESTATE->ShowW1() )
			score = TNS_W2;

		// Set TapNote score and offset
		if( score != TNS_None )
		{
			pTN->result.tns = score;
			pTN->result.fTapNoteOffset = -fNoteOffset;
		}

		// Handle attack notes
		if( pTN->type == TapNote::attack && score >= TNS_W2 )
		{
			// don't score this as anything
			score = TNS_None;

			// put attack in effect
			Attack attack( ATTACK_LEVEL_1, -1, pTN->fAttackDurationSeconds, pTN->sAttackModifiers, true, false );

			PlayerNumber pnToAttack = OPPOSITE_PLAYER [ m_pPlayerState->m_PlayerNumber ];
			PlayerState *pPlayerStateToAttack = GAMESTATE->m_pPlayerState [ pnToAttack ];
			pPlayerStateToAttack->LaunchAttack( attack );

			// remove all TapAttacks on this row
			for( int t = 0; t < m_NoteData.GetNumTracks(); t++ )
			{
				const TapNote &tn = m_NoteData.GetTapNote( t, iRowOfOverlappingNoteOrRow );
				if( tn.type == TapNote::attack )
					HideNote( t, iRowOfOverlappingNoteOrRow );
			}
		}

		//Handle Autosync if its enabled
		if( m_pPlayerState->m_PlayerController == PC_HUMAN && score >= TNS_W3 )
			AdjustSync::HandleAutosync( fNoteOffset, fStepSeconds );

		if( pTN->type != TapNote::mine )
		{
			if( score != TNS_None )
			{
				if( m_bCountNotesSeparately ) // Count notes separately? (Only for real judged notes)
				{
					if( score >= TNS_W3 )
					{
						if( m_pNoteField )
							m_pNoteField->DidTapNote( col, score, true );

						HideNote( col, iRowOfOverlappingNoteOrRow );
					}
					HandleTapRowScore( iRowOfOverlappingNoteOrRow, score );
				}
				else if( NoteDataWithScoring::IsRowCompletelyJudged( m_NoteData, iRowOfOverlappingNoteOrRow, pTN->nsp ))
				{
					FlashGhostRow( iRowOfOverlappingNoteOrRow, pTN->nsp );
				}
			}
		}
		else
		{
			if( m_pPrimaryScoreKeeper )
				m_pPrimaryScoreKeeper->HandleTapScore( *pTN );
		}
	}

	// We steped an arrow
	if( !bRelease && m_pNoteField )
	{
		m_pNoteField->Step( col, score );
	}

	//Enabled just to test infinity stuff
	if( !bRelease )
	{
		Message msg( "Step" );
		msg.SetParam( "PlayerNumber", m_pPlayerState->m_PlayerNumber );
		//msg.SetParam("MultiPlayer", mpPlayerState->m_mp);
		msg.SetParam( "Column", col );
		MESSAGEMAN->Broadcast( msg );
		// Backwards compatibility
		//Message msg2( "StepP%d", m_pPlayerState->m_PlayerNumber + 1) );
		//MESSAGEMAN->Broadcast( msg2 );
	}
}

void Player::Fret( int col, int row, const RageTimer &tm, bool bHeld, bool bRelease )
{
	if( IsOniDead() )
		return;

	DEBUG_ASSERT_M( col >= 0  &&  col <= m_NoteData.GetNumTracks(), ssprintf("%i, %i", col, m_NoteData.GetNumTracks()) );

	m_vbFretIsDown[ col ] = !bRelease;


	// Handle changing fret during a strum
	if( m_pPlayerState->m_fLastStrumMusicSeconds != -1 )
	{
		LOG->Trace( "StrumTry" );
		StepStrumHopo( col, row, tm, bHeld, bRelease, ButtonType_StrumFretsChanged );
	}


	// Check if this fret breaks all active holds.
	if( !bRelease )
	{
		const float fSongBeat = m_pPlayerState->m_Position.m_fSongBeat;
		const int iSongRow = BeatToNoteRow( fSongBeat );

		int iMaxHoldCol = -1;
		int iNumColsHeld = 0;

		// Score all active holds to NotHeld
		for( int iTrack=0; iTrack<m_NoteData.GetNumTracks(); ++iTrack )
		{
			// Since this is being called every frame, let's not check the whole array every time.
			// Instead, only check 1 beat back.  Even 1 is overkill.
			const int iStartCheckingAt = max( 0, iSongRow-BeatToNoteRow(1) );
			NoteData::TrackMap::iterator begin, end;
			m_NoteData.GetTapNoteRangeInclusive( iTrack, iStartCheckingAt, iSongRow+1, begin, end );
			for( ; begin != end; ++begin )
			{
				TapNote &tn = begin->second;
				if( tn.HoldResult.bActive )
				{
					iMaxHoldCol = iTrack;
					iNumColsHeld++;
				}
			}
		}

		// Any frets to the right of an active hold will break the hold.
		if( col > iMaxHoldCol  ||  iNumColsHeld >= 2 )
			ScoreAllActiveHoldsLetGo();
	}
}


void Player::Strum( int col, int row, const RageTimer &tm, bool bHeld, bool bRelease )
{
	if( bRelease )
		return;

	if( m_pPlayerState->m_fLastStrumMusicSeconds != -1 )
	{
		DoStrumMiss();
	}

	m_pPlayerState->m_fLastStrumMusicSeconds = m_pPlayerState->m_Position.m_fMusicSeconds;

	StepStrumHopo( col, row, tm, bHeld, bRelease, ButtonType_StrumFretsChanged );
}

void Player::DoTapScoreNone()
{
	Message msg( "ScoreNone" );
	MESSAGEMAN->Broadcast( msg );

	/* The only real way to tell if a mine has been scored is if it has disappeared
	* but this only works for hit mines so update the scores for avoided mines here. */
	if( m_pPrimaryScoreKeeper )
		m_pPrimaryScoreKeeper->HandleTapScoreNone();
	if( m_pSecondaryScoreKeeper )
		m_pSecondaryScoreKeeper->HandleTapScoreNone();


	if( m_pLifeMeter )
		m_pLifeMeter->HandleTapScoreNone();
	// TODO: Remove use of PlayerNumber
	PlayerNumber pn = PLAYER_INVALID;
	if( m_pCombinedLifeMeter )
		m_pCombinedLifeMeter->HandleTapScoreNone( pn );
}

void Player::DoStrumMiss()
{
	m_pPlayerState->m_fLastStrumMusicSeconds = -1;
	DoTapScoreNone();
}


void Player::StepStrumHopo( int col, int row, const RageTimer &tm, bool bHeld, bool bRelease, Player::ButtonType pbt )
{
	if( IsOniDead() )
		return;

	// Do everything that depends on a RageTimer here;
	// set your breakpoints somewhere after this block.
	const float fLastBeatUpdate = m_pPlayerState->m_Position.m_LastBeatUpdate.Ago();
	const float fPositionSeconds = m_pPlayerState->m_Position.m_fMusicSeconds - tm.Ago();
	const float fTimeSinceStep = tm.Ago();

	switch( pbt )
	{
	DEFAULT_FAIL(pbt);
	case ButtonType_Step:
		break;
	case ButtonType_StrumFretsChanged:
	case ButtonType_Hopo:
		// releasing should hit regular notes, not lifts
		bRelease = false;
		break;
	}

	float fSongBeat = m_pPlayerState->m_Position.m_fSongBeat;
	
	if( GAMESTATE->m_pCurSong )
	{
		fSongBeat = GAMESTATE->m_pCurSong->m_SongTiming.GetBeatFromElapsedTime( fPositionSeconds );
	
		if( GAMESTATE->m_pCurSteps[m_pPlayerState->m_PlayerNumber] )
			fSongBeat = m_Timing->GetBeatFromElapsedTime( fPositionSeconds );
	}
	
	const int iSongRow = row == -1 ? BeatToNoteRow( fSongBeat ) : row;

	if( col != -1 && !bRelease )
	{
		// Update roll life
		// Let's not check the whole array every time.
		// Instead, only check 1 beat back.  Even 1 is overkill.
		// Just update the life here and let Update judge the roll.
		const int iStartCheckingAt = max( 0, iSongRow-BeatToNoteRow(1) );
		NoteData::TrackMap::iterator begin, end;
		m_NoteData.GetTapNoteRangeInclusive( col, iStartCheckingAt, iSongRow+1, begin, end );
		for( ; begin != end; ++begin )
		{
			TapNote &tn = begin->second;
			if( tn.type != TapNote::hold_head )
				continue;

			switch( tn.subType )
			{
			DEFAULT_FAIL( tn.subType );
			case TapNote::hold_head_hold:
				continue;
			case TapNote::hold_head_roll:
				break;
			}

			const int iRow = begin->first;

			HoldNoteScore hns = tn.HoldResult.hns;
			if( hns != HNS_None )	// if this HoldNote already has a result
				continue;	// we don't need to update the logic for this one

			// if they got a bad score or haven't stepped on the corresponding tap yet
			const TapNoteScore tns = tn.result.tns;
			bool bInitiatedNote = true;
			bInitiatedNote = tns != TNS_None  &&  tns != TNS_Miss;	// did they step on the start?
			const int iEndRow = iRow + tn.iDuration;

			if( bInitiatedNote && tn.HoldResult.fLife != 0 )
			{
				/* This hold note is not judged and we stepped on its head.  Update iLastHeldRow.
				 * Do this even if we're a little beyond the end of the hold note, to make sure
				 * iLastHeldRow is clamped to iEndRow if the hold note is held all the way. */
				//LOG->Trace("setting iLastHeldRow to min of iSongRow (%i) and iEndRow (%i)",iSongRow,iEndRow);
				tn.HoldResult.iLastHeldRow = min( iSongRow, iEndRow );
			}

			// If the song beat is in the range of this hold:
			if( iRow <= iSongRow && iRow <= iEndRow )
			{
				if( bInitiatedNote )
				{
					// Increase life
					tn.HoldResult.fLife = 1;

					{
						// increment combo
						if( m_pPlayerStageStats )
						{
							m_pPlayerStageStats->m_iCurCombo++;
							m_pPlayerStageStats->m_iCurMissCombo = 0;
						}

						if( m_pPlayerStageStats )
							SetCombo( m_pPlayerStageStats->m_iCurCombo, m_pPlayerStageStats->m_iCurMissCombo );

						bool bBright = m_pPlayerStageStats && m_pPlayerStageStats->m_iCurCombo;
						if( m_pNoteField )
							m_pNoteField->DidHoldNote( col, HNS_Held, bBright );
					}
				}
				break;
			}
		}
	}

	// Count calories for this step, unless we're being called because a button
	// is held over a mine or being released.
	// TODO: Move calorie counting into a ScoreKeeper?
	if( m_pPlayerStageStats && m_pPlayerState && !bHeld && !bRelease )
	{
		// TODO: remove use of PlayerNumber
		PlayerNumber pn = m_pPlayerState->m_PlayerNumber;
		Profile *pProfile = PROFILEMAN->GetProfile( pn );

		int iNumTracksHeld = 0;
		for( int t=0; t<m_NoteData.GetNumTracks(); t++ )
		{
			GameInput GameI = GAMESTATE->GetCurrentStyle()->StyleInputToGameInput( t, pn );
			const float fSecsHeld = INPUTMAPPER->GetSecsHeld( GameI );
			if( fSecsHeld > 0  && fSecsHeld < m_fTimingWindowJump )
				iNumTracksHeld++;
		}

		float fCals = 0;
		switch( iNumTracksHeld )
		{
		case 0:
			// autoplay is on, or this is a computer player
			iNumTracksHeld = 1;
			// fall through
		default:
			{
				float fCalsFor100Lbs = SCALE( iNumTracksHeld, 1, 2, 0.023f, 0.077f );
				float fCalsFor200Lbs = SCALE( iNumTracksHeld, 1, 2, 0.041f, 0.133f );
				fCals = SCALE( pProfile->GetCalculatedWeightPounds(), 100.f, 200.f, fCalsFor100Lbs, fCalsFor200Lbs );
			}
			break;
		}

		m_pPlayerStageStats->m_fCaloriesBurned += fCals;
		m_pPlayerStageStats->m_iNumControllerSteps ++;
	}

	// Check for step on a TapNote
	/* XXX: This seems wrong. If a player steps twice quickly and two notes are
	 * close together in the same column then it is possible for the two notes
	 * to be graded out of order.
	 * Two possible fixes:
	 * 1. Adjust the fSongBeat (or the resulting note row) backward by
	 * iStepSearchRows and search forward two iStepSearchRows lengths,
	 * disallowing graded. This doesn't seem right because if a second note has
	 * passed, an earlier one should not be graded.
	 * 2. Clamp the distance searched backward to the previous row graded.
	 * Either option would fundamentally change the grading of two quick notes
	 * "jack hammers." Hmm.
	 */
	const int iStepSearchRows = max(
		BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds ) ) - iSongRow,
		iSongRow - BeatToNoteRow( m_Timing->GetBeatFromElapsedTime( m_pPlayerState->m_Position.m_fMusicSeconds ) )
	) + ROWS_PER_BEAT;
	int iRowOfOverlappingNoteOrRow = row;
	if( row == -1 )
	{
		switch( pbt )
		{
		DEFAULT_FAIL(pbt);
		case ButtonType_StrumFretsChanged:
		case ButtonType_Hopo:
		case ButtonType_Step:
			iRowOfOverlappingNoteOrRow = GetClosestNote( col, iSongRow, iStepSearchRows, iStepSearchRows, false );
			break;
		}
	}

	// calculate TapNoteScore
	TapNoteScore score = TNS_None;

	if( iRowOfOverlappingNoteOrRow != -1 )
	{
		// compute the score for this hit
		float fNoteOffset = 0.0f;
		// we need this later if we are autosyncing
		const float fStepBeat = NoteRowToBeat( iRowOfOverlappingNoteOrRow );
		const float fStepSeconds = m_Timing->GetElapsedTimeFromBeat(fStepBeat);

		if( row == -1 )
		{
			// We actually stepped on the note this long ago:
			//fTimeSinceStep

			/* GAMESTATE->m_fMusicSeconds is the music time as of GAMESTATE->m_LastBeatUpdate. Figure
			 * out what the music time is as of now. */
			const float fCurrentMusicSeconds = m_pPlayerState->m_Position.m_fMusicSeconds + (fLastBeatUpdate*GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate);

			// ... which means it happened at this point in the music:
			const float fMusicSeconds = fCurrentMusicSeconds - fTimeSinceStep * GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate;

			// The offset from the actual step in seconds:
			fNoteOffset = (fStepSeconds - fMusicSeconds) / GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate;	// account for music rate
			/*
			LOG->Trace("step was %.3f ago, music is off by %f: %f vs %f, step was %f off", 
				fTimeSinceStep, GAMESTATE->m_LastBeatUpdate.Ago()/GAMESTATE->m_SongOptions.m_fMusicRate,
				fStepSeconds, fMusicSeconds, fNoteOffset );
			*/
		}

		const float fSecondsFromExact = fabsf( fNoteOffset );

		TapNote tnDummy = TAP_ORIGINAL_TAP;
		TapNote *pTN = NULL;
		switch( pbt )
		{
		DEFAULT_FAIL(pbt);
		case ButtonType_StrumFretsChanged:
			pTN = &tnDummy;
			break;
		case ButtonType_Hopo:
		case ButtonType_Step:
			NoteData::iterator iter = m_NoteData.FindTapNote( col, iRowOfOverlappingNoteOrRow );
			DEBUG_ASSERT( iter!= m_NoteData.end(col) );
			pTN = &iter->second;
			break;
		}

		switch( m_pPlayerState->m_PlayerController )
		{
		case PC_HUMAN:
			switch( pTN->type )
			{
			case TapNote::mine:
				// Stepped too close to mine?
				if( !bRelease &&
				   fSecondsFromExact <= GetWindowSeconds(TW_Mine) &&
				   m_Timing->IsJudgableAtRow(iSongRow))
					score = TNS_HitMine;   
				break;
			case TapNote::attack:
				if( !bRelease && fSecondsFromExact <= GetWindowSeconds(TW_Attack) && !pTN->result.bHidden )
					score = AllowW1() ? TNS_W1 : TNS_W2; // sentinel
				break;
			case TapNote::hold_head:
				// oh wow, this was causing the trigger before the hold heads
				// bug. (It was fNoteOffset > 0.f before) -DaisuMaster

					score = TNS_W1;

				// Fall through to default.
			default:
				if( (pTN->type == TapNote::lift) == bRelease )
				{
					if(	 fSecondsFromExact <= GetWindowSeconds(TW_W1) )	score = TNS_W1;
					else if( fSecondsFromExact <= GetWindowSeconds(TW_W2) )	score = TNS_W2;
					else if( fSecondsFromExact <= GetWindowSeconds(TW_W3) )	score = TNS_W3;
					else if( fSecondsFromExact <= GetWindowSeconds(TW_W4) )	score = TNS_W4;
					else if( fSecondsFromExact <= GetWindowSeconds(TW_W5) )	score = TNS_W5;
				}
				break;
			}
			break;

		case PC_CPU:
		case PC_AUTOPLAY:
			/* XXX: This doesn't make sense.
			 * Step should only be called in autoplay for hit notes. */
#if 0
			// GetTapNoteScore always returns TNS_W1 in autoplay.
			// If the step is far away, don't judge it.
			if( m_pPlayerState->m_PlayerController == PC_AUTOPLAY &&
				fSecondsFromExact > GetWindowSeconds(TW_W5) )
			{
				score = TNS_None;
				break;
			}
#endif

			// TRICKY:  We're asking the AI to judge mines. Consider TNS_W4 and
			// below as "mine was hit" and everything else as "mine was avoided"
			if( pTN->type == TapNote::mine )
			{
				// The CPU hits a lot of mines. Only consider hitting the
				// first mine for a row. We know we're the first mine if 
				// there are are no mines to the left of us.
				for( int t=0; t<col; t++ )
				{
					if( m_NoteData.GetTapNote(t,iRowOfOverlappingNoteOrRow).type == TapNote::mine )	// there's a mine to the left of us
						return;	// avoid
				}

				// The CPU hits a lot of mines. Make it less likely to hit 
				// mines that don't have a tap note on the same row.
				bool bTapsOnRow = m_NoteData.IsThereATapOrHoldHeadAtRow( iRowOfOverlappingNoteOrRow );
				TapNoteScore get_to_avoid = bTapsOnRow ? TNS_W3 : TNS_W4;

				if( score >= get_to_avoid )
					return;	// avoided
				else
					score = TNS_HitMine;
			}

			if( pTN->type == TapNote::attack && score > TNS_W4 )
				score = TNS_W2; // sentinel

			/* AI will generate misses here. Don't handle a miss like a regular
			 * note because we want the judgment animation to appear delayed.
			 * Instead, return early if AI generated a miss, and let
			 * UpdateTapNotesMissedOlderThan() detect and handle the misses. */
			if( score == TNS_Miss )
				return;

			// Put some small, random amount in fNoteOffset so that demonstration 
			// show a mix of late and early. - Chris (StepMania r15628)
			//fNoteOffset = randomf( -0.1f, 0.1f );
			// Since themes may use the offset in a visual graph, the above
			// behavior is not the best thing to do. Instead, random numbers
			// should be generated based on the TapNoteScore, so that they can
			// logically match up with the current timing windows. -aj
			{
				float fWindowW1 = GetWindowSeconds(TW_W1);
				float fWindowW2 = GetWindowSeconds(TW_W2);
				float fWindowW3 = GetWindowSeconds(TW_W3);
				float fWindowW4 = GetWindowSeconds(TW_W4);
				float fWindowW5 = GetWindowSeconds(TW_W5);

				// W1 is the top judgment, there is no overlap.
				if( score == TNS_W1 )
					fNoteOffset = randomf(-fWindowW1, fWindowW1);
				else
				{
					// figure out overlap.
					float fLowerBound = 0.0f; // negative upper limit
					float fUpperBound = 0.0f; // positive lower limit
					float fCompareWindow = 0.0f; // filled in here:
					if( score == TNS_W2 )
					{
						fLowerBound = -fWindowW1;
						fUpperBound = fWindowW1;
						fCompareWindow = fWindowW2;
					}
					else if( score == TNS_W3 )
					{
						fLowerBound = -fWindowW2;
						fUpperBound = fWindowW2;
						fCompareWindow = fWindowW3;
					}
					else if( score == TNS_W4 )
					{
						fLowerBound = -fWindowW3;
						fUpperBound = fWindowW3;
						fCompareWindow = fWindowW4;
					}
					else if( score == TNS_W5 )
					{
						fLowerBound = -fWindowW4;
						fUpperBound = fWindowW4;
						fCompareWindow = fWindowW5;
					}
					float f1 = randomf(-fCompareWindow, fLowerBound);
					float f2 = randomf(fUpperBound, fCompareWindow);

					if(randomf() * 100 >= 50)
						fNoteOffset = f1;
					else
						fNoteOffset = f2;
				}
			}

			break;

		/*
		case PC_REPLAY:
			// based on where we are, see what grade to get.
			score = PlayerAI::GetTapNoteScore( m_pPlayerState );
			// row is the current row, col is current column (track)
			fNoteOffset = TapNoteOffset attribute
			break;
		*/
		default:
			FAIL_M(ssprintf("Invalid player controller type: %i", m_pPlayerState->m_PlayerController));
		}

		switch( pbt )
		{
		DEFAULT_FAIL(pbt);
		case ButtonType_StrumFretsChanged:
			{
				bool bNoteRowMatchesFrets = true;
				int iFirstNoteCol = -1;
				for( int i=0; i<GAMESTATE->GetCurrentStyle()->m_iColsPerPlayer; i++ )
				{
					const TapNote &tn = m_NoteData.GetTapNote( i, iRowOfOverlappingNoteOrRow );
					bool bIsNote = (tn.type != TapNote::empty);
					if( iFirstNoteCol == -1  &&  bIsNote )
						iFirstNoteCol = i;

					// Extra notes to the left (higher up on the string) can be held without penalty.  It's necessary to hold
					// the extra frets or pull-offs.
					if( iFirstNoteCol == -1 )
						continue;

					bool bNoteMatchesFret = m_vbFretIsDown[i] == bIsNote;
					if( !bNoteMatchesFret )
					{
						bNoteRowMatchesFrets = false;
						break;
					}
				}
				ASSERT( iFirstNoteCol != -1 );
				if( !bNoteRowMatchesFrets )
				{
					score = TNS_None;
				}
				else
				{
					int iLastNoteCol = -1;
					for( int i=GAMESTATE->GetCurrentStyle()->m_iColsPerPlayer-1; i>=0; i-- )
					{
						const TapNote &tn = m_NoteData.GetTapNote( i, iRowOfOverlappingNoteOrRow );
						bool bIsNote = (tn.type != TapNote::empty);
						if( bIsNote )
						{
							iLastNoteCol = i;
							break;
						}
					}

					m_pPlayerState->m_fLastHopoNoteMusicSeconds = fStepSeconds;
					m_pPlayerState->m_iLastHopoNoteCol = iLastNoteCol; 
				}
			}
			break;
		case ButtonType_Hopo:
			{
				// only can hopo on a row with one note
				if( m_NoteData.GetNumTapNotesInRow(iRowOfOverlappingNoteOrRow) != 1 )
				{
					score = TNS_None;
					break;
				}

				// con't hopo on the same note 2x in a row
				if( col == m_pPlayerState->m_iLastHopoNoteCol )
				{
					score = TNS_None;
					break;
				}

				const TapNote &tn = m_NoteData.GetTapNote( col, iRowOfOverlappingNoteOrRow );
				ASSERT( tn.type != TapNote::empty );

				int iRowsAgoLastNote = 100000;	// TODO: find more reasonable value based on HOPO_CHAIN_SECONDS?
				NoteData::all_tracks_reverse_iterator iter = m_NoteData.GetTapNoteRangeAllTracksReverse( iRowsAgoLastNote-iRowsAgoLastNote, iRowOfOverlappingNoteOrRow-1 );
				ASSERT( !iter.IsAtEnd() );	// there must have been a note that started the hopo

				const TapNoteResult &lastTNR = NoteDataWithScoring::LastTapNoteWithResult( m_NoteData, iter.Row() ).result;
				if( lastTNR.tns <= TNS_Miss )
				{
					score = TNS_None;
					break;
				}

				m_pPlayerState->m_fLastHopoNoteMusicSeconds = fStepSeconds;
				m_pPlayerState->m_iLastHopoNoteCol = col;
			}
			break;
		case ButtonType_Step:
			break;
		}

		// handle attack notes
		if( pTN->type == TapNote::attack && score == TNS_W2 )
		{
			score = TNS_None;	// don't score this as anything



			// put attack in effect
			Attack attack(
				ATTACK_LEVEL_1,
				-1,	// now
				pTN->fAttackDurationSeconds,
				pTN->sAttackModifiers,
				true,
				false
				);

			// TODO: Remove use of PlayerNumber
			PlayerNumber pnToAttack = OPPOSITE_PLAYER[m_pPlayerState->m_PlayerNumber];
			PlayerState *pPlayerStateToAttack = GAMESTATE->m_pPlayerState[pnToAttack];
			pPlayerStateToAttack->LaunchAttack( attack );

			// remove all TapAttacks on this row
			for( int t=0; t<m_NoteData.GetNumTracks(); t++ )
			{
				const TapNote &tn = m_NoteData.GetTapNote( t, iRowOfOverlappingNoteOrRow );
				if( tn.type == TapNote::attack )
					HideNote( t, iRowOfOverlappingNoteOrRow );
			}
		}

		if( m_pPlayerState->m_PlayerController == PC_HUMAN && score >= TNS_W3 ) 
			AdjustSync::HandleAutosync( fNoteOffset, fStepSeconds );

		// Do game-specific and mode-specific score mapping.
		score = GAMESTATE->GetCurrentGame()->MapTapNoteScore( score );
		if( score == TNS_W1 && !GAMESTATE->ShowW1() )
			score = TNS_W2;


		if( score != TNS_None )
		{
			switch( pbt )
			{
			DEFAULT_FAIL(pbt);
			case ButtonType_StrumFretsChanged:
				for( int t=0; t<m_NoteData.GetNumTracks(); t++ )
				{
					TapNote tn = m_NoteData.GetTapNote( t, iRowOfOverlappingNoteOrRow );
					if( tn.type != TapNote::empty )
					{
						tn.result.tns = score;
						tn.result.fTapNoteOffset = -fNoteOffset;
						m_NoteData.SetTapNote( t, iRowOfOverlappingNoteOrRow, tn );
					}
				}
				break;
			case ButtonType_Hopo:
			case ButtonType_Step:
				pTN->result.tns = score;
				pTN->result.fTapNoteOffset = -fNoteOffset;
				break;
			}
		}

		m_LastTapNoteScore = score;
		if( GAMESTATE->GetCurrentGame()->m_bCountNotesSeparately )
		{
			if( pTN->type != TapNote::mine )
			{
				const bool bBlind = (m_pPlayerState->m_PlayerOptions.GetCurrent().m_fBlind != 0);
				// XXX: This is the wrong combo for shared players.
				// STATSMAN->m_CurStageStats.m_Player[pn] might work, but could be wrong.
				const bool bBright = ( m_pPlayerStageStats && m_pPlayerStageStats->m_iCurCombo ) || bBlind;
				if( m_pNoteField )
					m_pNoteField->DidTapNote( col, bBlind? TNS_W1:score, bBright );
				if( score >= TNS_W3 || bBlind )
					HideNote( col, iRowOfOverlappingNoteOrRow );
			}
		}
	}

	// check for hopo end
	if( score <= TNS_Miss )
	{
		m_pPlayerState->ClearHopoState();
	}

	// check for strum end
	if( score != TNS_None )
	{
		switch( pbt )
		{
		DEFAULT_FAIL(pbt);
		case ButtonType_Step:
			break;
		case ButtonType_StrumFretsChanged:
			m_pPlayerState->m_fLastStrumMusicSeconds = -1;
			break;
		case ButtonType_Hopo:
			break;
		}
	}

	if( score == TNS_None )
	{
		switch( pbt )
		{
		DEFAULT_FAIL(pbt);
		case ButtonType_Step:
			DoTapScoreNone();
			break;
		case ButtonType_StrumFretsChanged:
		case ButtonType_Hopo:
			break;
		}

	}

	if( !bRelease )
	{
		/* Search for keyed sounds separately.  Play the nearest note. */
		/* XXX: This isn't quite right. As per the above XXX for iRowOfOverlappingNote, if iRowOfOverlappingNote
		 * is set to a previous note, the keysound could have changed and this would cause the wrong one to play,
		 * in essence playing two sounds in the opposite order. Maybe this should always perform the search. Still,
		 * even that doesn't seem quite right since it would then play the same (new) keysound twice which would
		 * sound wrong even though the notes were judged as being correct, above. Fixing the above problem would
		 * fix this one as well. */
		int iHeadRow;
		if( iRowOfOverlappingNoteOrRow != -1 && score != TNS_None )
		{
			// just pressing a note, use that row.
			// in other words, iRowOfOverlappingNoteOrRow = iRowOfOverlappingNoteOrRow
		}
		else if ( m_NoteData.IsHoldNoteAtRow( col, iSongRow, &iHeadRow ) )
		{
			// stepping on a hold, use it!
			iRowOfOverlappingNoteOrRow = iHeadRow;
		}
		else
		{
			// or else find the closest note.
			iRowOfOverlappingNoteOrRow = GetClosestNote( col, iSongRow, MAX_NOTE_ROW, MAX_NOTE_ROW, true );
		}
		if( iRowOfOverlappingNoteOrRow != -1 )
		{
			switch( pbt )
			{
			DEFAULT_FAIL(pbt);
			case ButtonType_StrumFretsChanged:
				for( int i=0; i<m_NoteData.GetNumTracks(); i++ )
				{
					const TapNote &tn = m_NoteData.GetTapNote( i, iRowOfOverlappingNoteOrRow );
					PlayKeysound( tn, score );
				}
				break;
			case ButtonType_Step:
			case ButtonType_Hopo:
				const TapNote &tn = m_NoteData.GetTapNote( col, iRowOfOverlappingNoteOrRow );
				PlayKeysound( tn, score );
				break;
			}
			
		}
	}
	// XXX:
	if( !bRelease )
	{
		if( m_pNoteField )
		{
			switch( pbt )
			{
			DEFAULT_FAIL(pbt);
			case ButtonType_StrumFretsChanged:
				{
					// only pulse the shortest string fret
					int iLastFret = -1;
					for( int i=0; i<m_NoteData.GetNumTracks(); i++ )
					{
						if( m_vbFretIsDown[i] )
							iLastFret = i;
					}
					if( iLastFret != -1 ) 
						m_pNoteField->Step( iLastFret, score );
				}
				break;
			case ButtonType_Step:
				m_pNoteField->Step( col, score );
				break;
			case ButtonType_Hopo:
				// no animation
				break;
			}
		}
		Message msg( "Step" );
		msg.SetParam( "PlayerNumber", m_pPlayerState->m_PlayerNumber );
		msg.SetParam( "MultiPlayer", m_pPlayerState->m_mp );
		msg.SetParam( "Column", col );
		MESSAGEMAN->Broadcast( msg );
		// Backwards compatibility
		Message msg2( ssprintf("StepP%d", m_pPlayerState->m_PlayerNumber + 1) );
		MESSAGEMAN->Broadcast( msg2 );
	}
}

void Player::UpdateTapNotesMissedOlderThan( float fMissIfOlderThanSeconds )
{
	//LOG->Trace( "Steps::UpdateTapNotesMissedOlderThan(%f)", fMissIfOlderThanThisBeat );
	int iMissIfOlderThanThisRow;
	const float fEarliestTime = m_pPlayerState->m_Position.m_fMusicSeconds - fMissIfOlderThanSeconds;
	{
		bool bFreeze, bDelay;
		float fMissIfOlderThanThisBeat;
		float fThrowAway;
		int iWarpBeginRow;
		float fWarpLength;
		m_Timing->GetBeatAndBPSFromElapsedTime( fEarliestTime, fMissIfOlderThanThisBeat, fThrowAway, bFreeze, bDelay, iWarpBeginRow, fWarpLength );

		iMissIfOlderThanThisRow = BeatToNoteRow( fMissIfOlderThanThisBeat );
		if( bFreeze || bDelay )
		{
			/* If there is a freeze on iMissIfOlderThanThisIndex, include this index too.
			 * Otherwise we won't show misses for tap notes on freezes until the
			 * freeze finishes. */
			if( !bDelay )
				iMissIfOlderThanThisRow++;
		}
	}

	NoteData::all_tracks_iterator &iter = *m_pIterNeedsTapJudging;

	for( ; !iter.IsAtEnd() && iter.Row() < iMissIfOlderThanThisRow; ++iter )
	{
		TapNote &tn = *iter;

		if( !NeedsTapJudging(tn) )
			continue;

		// Ignore all notes in WarpSegments or FakeSegments.
		if (!m_Timing->IsJudgableAtRow(iter.Row()))
			continue;

		if( tn.type == TapNote::mine )
		{
			tn.result.tns = TNS_AvoidMine;

			/* The only real way to tell if a mine has been scored is if it has disappeared
			 * but this only works for hit mines so update the scores for avoided mines here. */
			if( m_pPrimaryScoreKeeper )
				m_pPrimaryScoreKeeper->HandleTapScore( tn );
			if( m_pSecondaryScoreKeeper )
				m_pSecondaryScoreKeeper->HandleTapScore( tn );
		}
		else
		{
			tn.result.tns = TNS_Miss;
		}
	}
}

void Player::FlashGhostRow( int iRow, int iNSP )
{
	TapNoteScore lastTNS = NoteDataWithScoring::LastTapNoteWithResult( m_NoteData, iRow ).result.tns;
	const bool bBlind = (m_pPlayerState->m_PlayerOptions.GetCurrent().m_fBlind != 0);
	const bool bBright = ( m_pPlayerStageStats && m_pPlayerStageStats->m_iCurCombo ) || bBlind;

	for( int iTrack = 0; iTrack < m_NoteData.GetNumTracks(); ++iTrack )
	{
		const TapNote &tn = m_NoteData.GetTapNote( iTrack, iRow );

		if( tn.type == TapNote::empty || tn.type == TapNote::mine || tn.type == TapNote::fake )
			continue;
		if( m_pNoteField )
			m_pNoteField->DidTapNote( iTrack, lastTNS, bBright );
		if( lastTNS >= TNS_W3 || bBlind )
			HideNote( iTrack, iRow );
	}
}

void Player::CrossedRows( int iLastRowCrossed, const RageTimer &now )
{
	NoteData::all_tracks_iterator &iter = *m_pIterUncrossedRows;
	bool bIsJudgableAtRow = true;
	int iLastSeenRow = -1;

	for( ; !iter.IsAtEnd()  &&  iter.Row() <= iLastRowCrossed; ++iter )
	{
		TapNote &tn = *iter;
		int iRow = iter.Row();


		// Check if this row can be judged or not. Check it only once per row
		if (iLastSeenRow != iRow)
		{
			iLastSeenRow = iRow;
			bIsJudgableAtRow = this->m_Timing->IsJudgableAtRow(iRow);
		}

		// Ignore fake notes
		// Ignore note during fake or warp segments, but not holds
		if (tn.type == TapNote::fake || bIsJudgableAtRow) // Change to tn.judge later, there are many files to change too.
		{
			continue;
		}

		switch( tn.type )
		{
			case TapNote::autoKeysound:
			case TapNote::hold_tail:
			case TapNote::empty:
			case TapNote::mine:
			case TapNote::hold_head:
				continue;
				break;
			/*case TapNote::autoKeysound:
			{
			// handle autokeysounds here (if not in the editor).
			if( !GAMESTATE->m_bInStepEditor )
			{
				PlayKeysound(tn, TNS_None);
			}
			}; break;*/
			default:
			{
				if( m_pPlayerState->m_PlayerController != PC_HUMAN )
				{


					STATSMAN->m_CurStageStats.m_bUsedAutoplay = true;
					if (m_pPlayerStageStats && !(m_pPlayerStageStats->m_bDisqualified))
					{
						m_pPlayerStageStats->m_bDisqualified = true;
					}
				}
			}
			break;
		}
	}
}
void Player::CrossedHoldsRows ( int iLastRowCrossed, const RageTimer &now, float fDeltaTime )
{
	NoteData::all_tracks_iterator &iter = *m_pIterNeedsHoldJudging;
	bool bIsJudgableAtRow = true;
	int iLastSeenRow = -1;

	for( ; !iter.IsAtEnd () && iter.Row () <= iLastRowCrossed; ++iter )
	{
		TapNote &tn = *iter;
		int iRow = iter.Row ();
		int iTrack = iter.Track ();

		// Check if this row can be judged or not. Check it only once per row
		if( iLastSeenRow != iRow )
		{
			iLastSeenRow = iRow;
			bIsJudgableAtRow = this->m_Timing->IsJudgableAtRow ( iRow );
		}

		// Ignore fake notes
		// Ignore notes during fake or warp segmentos, BUT NO THE HOLDS!
		if( tn.judge == TapNote::fake || ( tn.type != TapNote::hold_head && !bIsJudgableAtRow ) )
		{
			continue;
		}

		switch( tn.type )
		{
			case TapNote::hold_head:
			{
				if( NeedsHoldJudging ( tn ) )
				{
					// Add a hold tail only if its a normal hold
					/*if( tn.subType == TapNote::hold_head_hold )
					{
						TapNote tail = tn;
						tail.type = TapNote::hold_tail;
						m_NoteData.SetTapNote( iTrack, iRow + tn.iDuration, tail );
					}*/

					TrackRowTapNote trtn = { iTrack, iRow, &tn };
					vHoldNotesToUpdate.push_back ( trtn );
					//LOG->Trace( "Player::TRTN added at row %d, track %d", iRow, iTrack );
				};
			}; break;
			default:
				continue;
				break;
		}
	}

	//
	// Check if theres any holds to update
	if( vHoldNotesToUpdate.empty () )
		return;

}


void Player::HandleTapRowScore( unsigned row, TapNoteScore tns )
{
	bool bNoCheating = true;
#ifdef DEBUG
	bNoCheating = false;
#endif

	// Do not score rows in WarpSegments or FakeSegments
	if (!m_Timing->IsJudgableAtRow(row))
		return;

	if( GAMESTATE->m_bDemonstrationOrJukebox )
		bNoCheating = false;
	// don't accumulate points if AutoPlay is on.
	if( bNoCheating && m_pPlayerState->m_PlayerController == PC_AUTOPLAY )
		return;

	TapNoteScore scoreOfLastTap = NoteDataWithScoring::LastTapNoteWithResult(m_NoteData, row).result.tns;

	if( scoreOfLastTap == TNS_Miss )
		m_LastTapNoteScore = TNS_Miss;

	for( int track = 0; track < m_NoteData.GetNumTracks(); ++track )
	{
		const TapNote &tn = m_NoteData.GetTapNote( track, row );
		// Mines cannot be handled here.
		if (tn.type == TapNote::empty ||
			tn.type == TapNote::fake ||
			tn.type == TapNote::mine ||
			tn.type == TapNote::autoKeysound)
			continue;
		if( m_pPrimaryScoreKeeper )
			m_pPrimaryScoreKeeper->HandleTapScore( tn );
		if( m_pSecondaryScoreKeeper )
			m_pSecondaryScoreKeeper->HandleTapScore( tn );
	}

	if( m_pPrimaryScoreKeeper != NULL )
		m_pPrimaryScoreKeeper->HandleTapRowScore( m_NoteData, row );
	if( m_pSecondaryScoreKeeper != NULL )
		m_pSecondaryScoreKeeper->HandleTapRowScore( m_NoteData, row );

	/* Use the real current beat, not the beat we've been passed. That's because
	 * we want to record the current life/combo to the current time; eg. if it's
	 * a MISS, the beat we're registering is in the past, but the life is changing
	 * now. We need to include time from previous songs in a course, so we
	 * can't use GAMESTATE->m_fMusicSeconds. Use fStepsSeconds instead. */
	if( m_pPlayerStageStats )
		m_pPlayerStageStats->UpdateComboList( STATSMAN->m_CurStageStats.m_fStepsSeconds, false );

	if( m_pScoreDisplay )
	{
		if( m_pPlayerStageStats )
			m_pScoreDisplay->SetScore( m_pPlayerStageStats->m_iScore );
		m_pScoreDisplay->OnJudgment( scoreOfLastTap );
	}
	if( m_pSecondaryScoreDisplay )
	{
		if( m_pPlayerStageStats )
			m_pSecondaryScoreDisplay->SetScore( m_pPlayerStageStats->m_iScore );
		m_pSecondaryScoreDisplay->OnJudgment( scoreOfLastTap );
	}

	ChangeLife( scoreOfLastTap );
}

void Player::HandleHoldCheckpoint(int iRow, 
				  int iNumHoldsHeldThisRow, 
				  int iNumHoldsMissedThisRow, 
				  const vector<int> &viColsWithHold, bool bHoldsAreBeingPressed )
{
	bool bNoCheating = true;
#ifdef DEBUG
	bNoCheating = false;
#endif

	// WarpSegments and FakeSegments aren't judged in any way.
	if (!m_Timing->IsJudgableAtRow(iRow))
		return;

	// don't accumulate combo if AutoPlay is on.
	if( bNoCheating && m_pPlayerState->m_PlayerController == PC_AUTOPLAY )
		return;

	if( m_pPrimaryScoreKeeper )
		m_pPrimaryScoreKeeper->HandleHoldCheckpointScore(m_NoteData, 
								 iRow, 
								 iNumHoldsHeldThisRow, 
								 iNumHoldsMissedThisRow );
	if( m_pSecondaryScoreKeeper )
		m_pSecondaryScoreKeeper->HandleHoldCheckpointScore(m_NoteData, 
								   iRow, 
								   iNumHoldsHeldThisRow, 
								   iNumHoldsMissedThisRow );

	if( m_pPlayerStageStats )
	{
		SetCombo( m_pPlayerStageStats->m_iCurCombo, m_pPlayerStageStats->m_iCurMissCombo );
		m_pPlayerStageStats->UpdateComboList( STATSMAN->m_CurStageStats.m_fStepsSeconds, false );
	}

	ChangeLife( iNumHoldsMissedThisRow == 0? TNS_CheckpointHit:TNS_CheckpointMiss );

	SetJudgment( iNumHoldsMissedThisRow == 0? TNS_CheckpointHit:TNS_CheckpointMiss, viColsWithHold[0], 0 );
}

void Player::HandleHoldScore( const TapNote &tn )
{
	HoldNoteScore holdScore = tn.HoldResult.hns;
	TapNoteScore tapScore = tn.result.tns;
	bool bNoCheating = true;
#ifdef DEBUG
	bNoCheating = false;
#endif

	if( GAMESTATE->m_bDemonstrationOrJukebox )
		bNoCheating = false;
	// don't accumulate points if AutoPlay is on.
	if( bNoCheating && m_pPlayerState->m_PlayerController == PC_AUTOPLAY )
		return;

	if( m_pPrimaryScoreKeeper )
		m_pPrimaryScoreKeeper->HandleHoldScore( tn );
	if( m_pSecondaryScoreKeeper )
		m_pSecondaryScoreKeeper->HandleHoldScore( tn );

	if( m_pScoreDisplay )
	{
		if( m_pPlayerStageStats ) 
			m_pScoreDisplay->SetScore( m_pPlayerStageStats->m_iScore );
		m_pScoreDisplay->OnJudgment( holdScore, tapScore );
	}
	if( m_pSecondaryScoreDisplay )
	{
		if( m_pPlayerStageStats ) 
			m_pSecondaryScoreDisplay->SetScore( m_pPlayerStageStats->m_iScore );
		m_pSecondaryScoreDisplay->OnJudgment( holdScore, tapScore );
	}


}

float Player::GetMaxStepDistanceSeconds()
{
	float fMax = abs(BAD_D);

	float f = GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate * fMax;
	return f + m_fMaxInputLatencySeconds;
}

void Player::FadeToFail()
{
	if( m_pNoteField )
		m_pNoteField->FadeToFail();

	// clear miss combo
	SetCombo( 0, 0 );
}

void Player::CacheAllUsedNoteSkins()
{
	if( m_pNoteField )
		m_pNoteField->CacheAllUsedNoteSkins();
}

void Player::SetJudgment( TapNoteScore tns, int iTrack, float fTapNoteOffset )
{
	if( m_bSendJudgmentAndComboMessages )
	{
		Message msg("Judgment");
		msg.SetParam( "Player", m_pPlayerState->m_PlayerNumber );
		msg.SetParam( "MultiPlayer", m_pPlayerState->m_mp );
		msg.SetParam( "FirstTrack", iTrack );
		msg.SetParam( "TapNoteScore", tns );
		msg.SetParam( "Early", fTapNoteOffset < 0.0f );
		msg.SetParam( "TapNoteOffset", fTapNoteOffset );
		MESSAGEMAN->Broadcast( msg );
	}
}

void Player::SetHoldJudgment( TapNoteScore tns, HoldNoteScore hns, int iTrack )
{
	ASSERT( iTrack < (int)m_vpHoldJudgment.size() );
	if( m_vpHoldJudgment[iTrack] )
		m_vpHoldJudgment[iTrack]->SetHoldJudgment( hns );

	if( m_bSendJudgmentAndComboMessages )
	{
		Message msg("Judgment");
		msg.SetParam( "Player", m_pPlayerState->m_PlayerNumber );
		msg.SetParam( "MultiPlayer", m_pPlayerState->m_mp );
		msg.SetParam( "FirstTrack", iTrack );
		msg.SetParam( "NumTracks", (int)m_vpHoldJudgment.size() );
		msg.SetParam( "TapNoteScore", tns );
		msg.SetParam( "HoldNoteScore", hns );
		MESSAGEMAN->Broadcast( msg );
	}
}

void Player::SetCombo( int iCombo, int iMisses )
{
	if( m_iLastSeenCombo == -1 )	// first update, don't set bIsMilestone=true
		m_iLastSeenCombo = iCombo;

	bool b25Milestone = false;
	bool b50Milestone = false;
	bool b100Milestone = false;
	bool b250Milestone = false;
	bool b1000Milestone = false;
	for( int i=m_iLastSeenCombo+1; i<=iCombo; i++ )
	{
		if( i < 600 )
		{
			b25Milestone |= ((i % 25) == 0);
			b50Milestone |= ((i % 50) == 0);
			b100Milestone |= ((i % 100) == 0);
			b250Milestone |= ((i % 250) == 0);
		}
		else
		{
			b1000Milestone |= ((i % 200) == 0);
		}
	}
	m_iLastSeenCombo = iCombo;

	if( b25Milestone )
		this->PlayCommand( "TwentyFiveMilestone");
	if( b50Milestone )
		this->PlayCommand( "FiftyMilestone");
	if( b100Milestone )
		this->PlayCommand( "HundredMilestone" );
	if( b250Milestone )
		this->PlayCommand( "TwoHundredFiftyMilestone");
	if( b1000Milestone )
		this->PlayCommand( "ThousandMilestone" );

	/* Colored combo logic differs between Songs and Courses.
	 *	Songs:
	 *	The theme decides how far into the song the combo color should appear.
	 *	(PERCENT_UNTIL_COLOR_COMBO)
	 *
	 *	Courses:
	 *	PERCENT_UNTIL_COLOR_COMBO refers to how long through the course the
	 *	combo color should appear (scaling to the number of songs). This may
	 *	not be desired behavior, however. -aj
	 *
	 *	TODO: Add a metric that determines Course combo colors logic?
	 *	Or possibly move the logic to a Lua function? -aj */
	bool bPastBeginning = false;

	if( m_bSendJudgmentAndComboMessages )
	{
		Message msg("Combo");
		if( iCombo )
			msg.SetParam( "Combo", iCombo );
		if( iMisses )
			msg.SetParam( "Misses", iMisses );
		if( bPastBeginning && m_pPlayerStageStats->FullComboOfScore(TNS_W1) )
			msg.SetParam( "FullComboW1", true );
		if( bPastBeginning && m_pPlayerStageStats->FullComboOfScore(TNS_W2) )
			msg.SetParam( "FullComboW2", true );
		if( bPastBeginning && m_pPlayerStageStats->FullComboOfScore(TNS_W3) )
			msg.SetParam( "FullComboW3", true );
		if( bPastBeginning && m_pPlayerStageStats->FullComboOfScore(TNS_W4) )
			msg.SetParam( "FullComboW4", true );
		this->HandleMessage( msg );
	}
}

RString Player::ApplyRandomAttack()
{
	if( GAMESTATE->m_RandomAttacks.size() < 1 )
		return "";

	//int iAttackToUse = rand() % GAMESTATE->m_RandomAttacks.size();
	DateTime now = DateTime::GetNowDate();
	int iSeed = now.tm_hour * now.tm_min * now.tm_sec * now.tm_mday;
	RandomGen rnd( GAMESTATE->m_iStageSeed * iSeed );
	int iAttackToUse = rnd() % GAMESTATE->m_RandomAttacks.size();
	return GAMESTATE->m_RandomAttacks[iAttackToUse];
}


/*
 * (c) 2001-2006 Chris Danford, Steve Checkoway
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
