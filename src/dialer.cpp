#include "dialer.h"
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <string>
#include <sstream>
#include <deque>
#include <cassert>
#include "sound.h"

using namespace std;
using namespace cv;

namespace {
const int window_width = 800;
const int window_height = 600;
const char *dialer_window_name = "dialer";
const int moving_average_size = 3;
const int ticks_per_seconds = 25;
const int debounce_delay_ticks = 8;
const int countdown_ticks = 74;
const float eye_movement_threashold = 0.06;
const int choices_gap_size = 150;

// convert integer to string
string str(int n)
{
	ostringstream ss;
	ss << n;
	return ss.str();
}
}

enum EyeMovement
{
	CENTER = 0,
	LEFT = 1,
	RIGHT = 2,
};

class DialerContext;

class State
{
public:
	virtual ~State() { }
	virtual void enter(DialerContext *ctx) {};
	virtual void exit(DialerContext *ctx) {};
	virtual void render(DialerContext *ctx) {};
	virtual void eyeMovement(DialerContext *ctx, EyeMovement movement) {};
	virtual void tick(DialerContext *ctx) {};
	virtual void commitChoice(DialerContext *ctx) {};
};

class InputState : public State
{
public:
	InputState();
	virtual void enter(DialerContext *ctx);
	virtual void render(DialerContext *ctx);
	virtual void eyeMovement(DialerContext *ctx, EyeMovement movement);
	virtual void commitChoice(DialerContext *ctx);
protected:
	Sound sound_select, sound_change;
};

class WaitState : public State
{
public:
	virtual void enter(DialerContext *ctx);
	virtual void render(DialerContext *ctx);
	virtual void eyeMovement(DialerContext *ctx, EyeMovement movement);
	virtual void tick(DialerContext *ctx);
private:
	EyeMovement prev_movement;
	int points;
};

class PhoneCallState : public State
{
public:
	PhoneCallState();
	~PhoneCallState();
	virtual void enter(DialerContext *ctx);
	virtual void render(DialerContext *ctx);
	virtual void tick(DialerContext *ctx);
private:
	int ticks;
	Sound sound_phone;
	Mat avatar;
};

// TODO: extract common base case with InputState
class ConfirmState : public InputState
{
public:
	virtual void enter(DialerContext *ctx);
	virtual void render(DialerContext *ctx);
	virtual void commitChoice(DialerContext *ctx);
};

class DialerContext
{
public:
	string input;
	Mat canvas;
	State *state;
	int current_choice_index;
	vector<string> choices;
	deque<float> position_history;
	int wait_ticks;
	int countdown;
	EyeMovement movement;

	DialerContext() : canvas(Mat::zeros(window_height, window_width, CV_8UC3)),
		state(NULL), current_choice_index(0), wait_ticks(0),
		countdown(countdown_ticks)
	{
		setState(new WaitState);
		assert(state != NULL);
	}

	~DialerContext()
	{
		state->exit(this);
		delete state;
	}

	void setState(State *new_state)
	{
		assert(new_state != NULL);
		if (new_state == state)
			return;

		if (state)
			state->exit(this);
		new_state->enter(this);

		delete state;
		state = new_state;
	}

	void setChoices(const vector<string>& new_choices)
	{
		choices = new_choices;
		current_choice_index = 0;
	}

	void clear()
	{
		canvas = CV_RGB(2, 23, 40);
	}

	void drawText(const string& s, int x, int y, Scalar color, double scale = 1.0)
	{
		const int thickness = 2;
		putText(canvas, s, Point(x, y), FONT_HERSHEY_SIMPLEX, scale, color,
				thickness);
	}

	void drawTextCentered(const string& s, int x, int y, Scalar color,
			double scale = 1.0)
	{
		const int thickness = 2;
		Size size = getTextSize(s, FONT_HERSHEY_SIMPLEX, scale, thickness, NULL);
		drawText(s, x - size.width / 2, y - size.height / 2, color, scale);
	}

	void show()
	{
		imshow(dialer_window_name, canvas);
	}

	void drawChoices()
	{
		const int center_x = window_width / 2, center_y = window_height / 2;
		bool is_moving = (movement != CENTER);
		int offset = is_moving ? (movingProgress() * choices_gap_size) : 0;

		if (movement == LEFT)
			offset = -offset;

		// draw the current choice at the center
		drawTextCentered(currentChoice(), center_x + offset, center_y,
				CV_RGB(255, 0, 0), is_moving ? 1.5 : 2.5);

		// draw the previous and next N choices
		for (int i = 1; i <= 3; i++) {
			int next_index = (current_choice_index + i) % choices.size();
			int prev_index = (current_choice_index + choices.size() - i) % choices.size();
			const string next_choice = choices[next_index];
			const string prev_choice = choices[prev_index];

			drawTextCentered(prev_choice, center_x - i * choices_gap_size + offset,
					center_y,
					CV_RGB(255, 0, 0), 1.5);
			drawTextCentered(next_choice, center_x + i * choices_gap_size + offset,
					center_y,
					CV_RGB(255, 0, 0), 1.5);
		}
	}

	void drawCountdown() {
		int seconds = countdown / ticks_per_seconds + 1;
		drawTextCentered(str(seconds), window_width / 2, window_height / 4,
				CV_RGB(0, 255, 0), 1.5);
	}

	void drawEyePositionIndicator()
	{
		float diff = getMovingAverage() - 0.5;
		int x = window_width * (diff + eye_movement_threashold) / (2*eye_movement_threashold);
		line(canvas, Point(x, 0), Point(x, 10), CV_RGB(255, 255, 255), 5);
	}

	void drawAll() {
		clear();
		state->render(this);
		drawEyePositionIndicator();
		show();
	}

	string currentChoice() {
		return choices[current_choice_index];
	}

	void selectNext() {
		current_choice_index = nextChoiceIndex();
	}

	void selectPrev() {
		current_choice_index = prevChoiceIndex();
	}

	int prevChoiceIndex() {
		return (current_choice_index + choices.size() - 1) % choices.size();
	}

	int nextChoiceIndex() {
		return (current_choice_index + 1) % choices.size();
	}

	float getMovingAverage()
	{
		if (position_history.empty())
			return 0.5;

		float sum = 0;
		for (int i = 0; i < position_history.size(); i++)
			sum += position_history[i];
		return sum / position_history.size();
	}

	void detectEyeMovement()
	{
		if (wait_ticks > 0) {
			wait_ticks--;
			return;
		}

		float diff = getMovingAverage() - 0.5;
		movement = CENTER;
		if (diff < -eye_movement_threashold)
			movement = LEFT;
		else if (diff > eye_movement_threashold)
			movement = RIGHT;

		state->eyeMovement(this, movement);

		if (movement != CENTER)
			wait_ticks = debounce_delay_ticks;
	}

	float movingProgress()
	{
		// fraction of the completed moving progress
		return (float)wait_ticks / debounce_delay_ticks;
	}

	// append s to input
	void inputPush(const string& s)
	{
		input += s;
	}

	// remove the last character from input
	void inputPop()
	{
		if (!input.empty())
			input.erase(input.size() - 1); // remove the last character
	}

	void checkCountdown()
	{
		if (countdown > 0)
			countdown--;
		else {
			countdown = countdown_ticks;
			state->commitChoice(this);
		}
	}

	void tick()
	{
		drawAll();
		checkCountdown();
		detectEyeMovement();
		state->tick(this);
	}

};

// InputState {{{

InputState::InputState()
	: sound_select("select.ogg"), sound_change("change.ogg")
{
}

void InputState::enter(DialerContext *ctx)
{
	vector<string> choices;

	// chocies: 0~9
	for (int i = 0; i <= 9; i++)
		choices.push_back(str(i));
	// delete backward
	choices.push_back("Del");
	choices.push_back("Call");

	ctx->setChoices(choices);
}

void InputState::render(DialerContext *ctx)
{
	ctx->drawText(ctx->input, 100, 100, CV_RGB(255, 0, 0));
	ctx->drawChoices();
	ctx->drawCountdown();
}

void InputState::eyeMovement(DialerContext *ctx, EyeMovement movement)
{
	if (movement == LEFT)
		ctx->selectPrev();
	else if (movement == RIGHT)
		ctx->selectNext();

	if (movement != CENTER) {
		ctx->countdown = countdown_ticks;
		sound_change.play();
	}
}

void InputState::commitChoice(DialerContext *ctx)
{
	sound_select.play();

	const string& choice = ctx->currentChoice();
	if (choice == "Del")
		ctx->inputPop();
	else if (choice == "Call")
		ctx->setState(new ConfirmState);
	else
		ctx->inputPush(choice);
}

// }}}

// WaitState {{{

void WaitState::enter(DialerContext *ctx)
{
	prev_movement = CENTER;
	points = 0;
	ctx->input.clear();
}

void WaitState::render(DialerContext *ctx)
{
	ctx->drawTextCentered("Quickly look left and right 5 times to start",
			window_width/2, window_height/2,
			CV_RGB(255, 255, 255));
}

void WaitState::eyeMovement(DialerContext *ctx, EyeMovement movement)
{
	// We are only interested in left and right in this state.
	if (movement == CENTER)
		return;

	if (prev_movement != movement) // transition from left to right or vice versa
		points += 1.5 * ticks_per_seconds;

	if (points >= ticks_per_seconds * 5) {
		Sound("select.ogg").play();
		ctx->setState(new InputState);
		return;
	}

	prev_movement = movement;
}

void WaitState::tick(DialerContext *ctx)
{
	if (points > 0)
		points--;
}

// }}}

// ConfirmState {{{

void ConfirmState::enter(DialerContext *ctx)
{
	vector<string> choices;
	choices.push_back("No");
	choices.push_back("Yes");
	choices.push_back("Back");
	ctx->setChoices(choices);
}

void ConfirmState::render(DialerContext *ctx)
{
	string msg("Do you want to call ");
	msg += ctx->input;

	ctx->drawTextCentered(msg, window_width/2, window_height/2 + 100,
			CV_RGB(255, 255, 0));
	ctx->drawChoices();
	ctx->drawCountdown();
}

void ConfirmState::commitChoice(DialerContext *ctx)
{
	sound_select.play();
	const string choice = ctx->currentChoice();
	if (choice == "Yes") {
		ctx->setState(new PhoneCallState);
	} else if (choice == "Back") {
		ctx->setState(new InputState);
	} else {
		ctx->setState(new WaitState);
	}
}

// }}}

// PhoneCallState {{{

PhoneCallState::PhoneCallState() : ticks(0), sound_phone("phone-call.ogg")
{
	avatar = imread("avatar.png", CV_LOAD_IMAGE_COLOR);
}

PhoneCallState::~PhoneCallState()
{
}

void PhoneCallState::enter(DialerContext *ctx)
{
	ticks = 10 * ticks_per_seconds;
	sound_phone.play();
}

void PhoneCallState::render(DialerContext *ctx)
{
	// display avatar
	const int avatar_width = avatar.cols, avatar_height = avatar.rows;
	const int avatar_left = window_width / 2 - avatar_width / 2;
	const int avatar_top = window_height / 2 - avatar_height / 2;
	Rect region(avatar_left, avatar_top, avatar_width, avatar_height);
	avatar.copyTo(ctx->canvas(region));

	// display phone number
	const string& number = ctx->input;
	ctx->drawTextCentered(number,
			window_width / 2, avatar_top + avatar_height + 50,
			CV_RGB(255, 255, 255));
}

void PhoneCallState::tick(DialerContext *ctx)
{
	if (--ticks == 0)
		ctx->setState(new WaitState);
}

// }}}

Dialer::Dialer() : ctx(new DialerContext)
{
}

Dialer::~Dialer()
{
	delete ctx;
}

void Dialer::start()
{
	namedWindow(dialer_window_name, CV_WINDOW_NORMAL);
	resizeWindow(dialer_window_name, window_width, window_height);
	moveWindow(dialer_window_name, 0, 0);
}

void Dialer::stop()
{
	destroyWindow(dialer_window_name);
}

void Dialer::keypress(int key)
{
	switch (key)
	{
		case 'h':
			ctx->selectNext();
			break;
		case 'l':
			ctx->selectPrev();
			break;
	}
}

void Dialer::tick()
{
	ctx->tick();
}

void Dialer::updatePupilPosition(float pupil_left_x, float pupil_left_y,
		float pupil_right_x, float pupil_right_y)
{
	const float position = (pupil_left_x + pupil_right_x) / 2;
	ctx->position_history.push_back(position);
	while (ctx->position_history.size() > moving_average_size)
		ctx->position_history.pop_front();
}
