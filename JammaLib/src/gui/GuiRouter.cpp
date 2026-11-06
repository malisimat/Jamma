#include "GuiRouter.h"
#include "GuiPanel.h"
#include "GuiButton.h"
#include "../graphics/GlDeleteQueue.h"

using namespace base;
using namespace gui;
using namespace utils;
using namespace actions;
using namespace resources;

class GuiRouter::GuiRouterChannel::Socket : public GuiElement
{
public:
	Socket(GuiElementParams params) : GuiElement(params) {}
	void Draw(DrawContext& ctx) override
	{
		auto& gl = dynamic_cast<graphics::GlDrawContext&>(ctx);
		const auto p = Position();
		const float scale = _highlighted ? 1.5f : 1.0f;
		gl.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(p.X + 4.0f, p.Y + 4.0f, 0.0f)) *
			glm::scale(glm::mat4(1.0f), glm::vec3(scale, scale, 1.0f)) *
			glm::translate(glm::mat4(1.0f), glm::vec3(-p.X - 4.0f, -p.Y - 4.0f, 0.0f)));
		GuiElement::Draw(ctx);
		gl.PopMvp();
	}
	void ApplyHoverState(bool inside) override
	{
		_highlighted = inside;
		_guiParams.TintColor = inside ? glm::vec3{1.0f, 0.90f, 0.35f} : glm::vec3{0.4f, 0.85f, 1.0f};
	}
private:
	bool _highlighted = false;
};

void GuiRouter::GuiRouterChannel::ApplyHoverState(bool inside)
{
	GuiElement::ApplyHoverState(inside);
	_socket->ApplyHoverState(inside);
}

void GuiRouter::GuiRouterChannel::ClearPointerState()
{
	GuiElement::ClearPointerState();
	_socket->ApplyHoverState(false);
}

Position2d GuiRouter::GuiRouterChannel::SocketPosition() const
{
	return Position() + _socket->Position() + Position2d{4, 4};
}

std::shared_ptr<GuiRouter::GuiRouterChannel> GuiRouter::_NearbyChannel(Position2d point, bool inputsOnly, bool outputsOnly) const
{
	std::shared_ptr<GuiRouterChannel> nearest;
	float distance = 18.0f * 18.0f;
	const auto visit = [&](const auto& channels) {
		for (const auto& channel : channels)
		{
			const auto delta = channel->SocketPosition() - point;
			const float squared = static_cast<float>(delta.X) * delta.X + static_cast<float>(delta.Y) * delta.Y;
			if (channel->HitTest(point - channel->Position())) { nearest = channel; distance = 0.0f; return nearest; }
			if (squared <= distance) { distance = squared; nearest = channel; }
		}
		return nearest;
	};
	if (!outputsOnly) visit(_inputs);
	if (!inputsOnly) visit(_outputs);
	return nearest;
}

void GuiRouter::_UpdateSocketHover(Position2d point)
{
	const auto nearby = _NearbyChannel(point, _isDragging && !_isDraggingInput, _isDragging && _isDraggingInput);
	for (const auto& channel : _inputs) channel->ApplyHoverState(channel == nearby);
	for (const auto& channel : _outputs) channel->ApplyHoverState(channel == nearby);
	_currentDragPos = nearby && _isDragging ? nearby->SocketPosition() : point;
}

const unsigned int GuiRouter::_MaxRoutes = 128;
const int GuiRouter::_WireYOffset = 6;

GuiRouter::GuiRouterChannel::GuiRouterChannel(GuiRouterChannelParams params) :
	GuiElement(params),
	_isActive(false),
	_activeTexture(graphics::ImageParams(DrawableParams{ params.ActiveTexture }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV)),
	_highlightTexture(graphics::ImageParams(DrawableParams{ params.HighlightTexture }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV)),
	_activeDownTexture(graphics::ImageParams(DrawableParams{ "rounded_but_on_down" }, SizeableParams{ params.Size,params.MinSize }, params.TextureShader, params.Rot90, params.FlipH, params.FlipV)),
	_label(nullptr),
	_params(params)
{
	const auto textStyle = GuiButtonParams::PanelButton();
	const auto frame = GuiLabelParams::ResolveTextFrame(params.Size.Width,
		params.Size.Height, textStyle.TextPadding, textStyle.TextPadding, true);
	GuiLabelParams labelParams;
	labelParams.String = std::to_string(params.Channel + 1u);
	labelParams.Position = {static_cast<int>(frame.PaddingX), frame.OffsetY};
	labelParams.Size = {frame.ContentWidth, frame.TextHeight};
	labelParams.MinSize = {1u, frame.TextHeight};
	labelParams.FontSize = textStyle.TextFontSize;
	labelParams.TextColor = textStyle.TextColor;
	labelParams.CenterHorizontally = true;
	labelParams.VerticalAlign = GuiTextVerticalAlign::Center;
	labelParams.ClipText = true;
	_label = std::make_shared<GuiLabel>(labelParams);
	_children.push_back(_label);
	GuiElementParams socket;
	socket.Size = {8u, 8u}; socket.MinSize = socket.Size;
	socket.Position = {static_cast<int>(params.Size.Width / 2u) - 4,
		params.IsInput ? 2 : static_cast<int>(params.Size.Height) - 10};
	socket.GuiPassThrough = true;
	socket.Texture = "hud_socket_cable";
	socket.TextureShader = "texture_tinted";
	socket.TintColor = {0.4f, 0.85f, 1.0f};
	_socket = std::make_shared<Socket>(socket);
	AddChild(_socket);
}

void GuiRouter::GuiRouterChannel::Draw(base::DrawContext& ctx)
{
	GuiElement::Draw(ctx);
}

graphics::Image& GuiRouter::GuiRouterChannel::_CurrentTexture()
{
	if (_isActive)
		return _state == STATE_DOWN ? _activeDownTexture : (_state == STATE_OVER ? _highlightTexture : _activeTexture);
	return GuiElement::_CurrentTexture();
}

ActionResult GuiRouter::GuiRouterChannel::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	if (res.IsEaten)
	{
		res.SourceId = ChanToString(_params.Channel);
		res.ResultType = _params.IsInput ?
			ACTIONRESULT_ROUTERINPUT :
			ACTIONRESULT_ROUTEROUTPUT;
	}

	return res;
}

void GuiRouter::GuiRouterChannel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);

	_activeTexture.InitResources(resourceLib, forceInit);
	_highlightTexture.InitResources(resourceLib, forceInit);
	_activeDownTexture.InitResources(resourceLib, forceInit);

	GlUtils::CheckError("GuiRouterChannel::_InitResources()");
}

const unsigned int GuiRouter::StringToChan(std::string id)
{
	if (id.empty())
		return 0;

	unsigned int num = 0;

	try {
		num = std::stoul(id);
	}
	catch (const std::invalid_argument& e) {
		std::cerr << "Invalid argument: " << e.what() << std::endl;
	}
	catch (const std::out_of_range& e) {
		std::cerr << "Out of range: " << e.what() << std::endl;
	}

	return num;
}

const std::string GuiRouter::ChanToString(unsigned int chan)
{
	return std::to_string(chan);
}

GuiRouter::GuiRouter(GuiRouterParams params,
	unsigned int numInputs,
	unsigned int numOutputs) :
	GuiElement(params),
	_vertexArray(0),
	_vertexBuffer(0),
	_isDragging(false),
	_isDraggingInput(true),
	_initDragChannel(0u),
	_currentDragPos{ 0,0 },
	_routerParams(params),
	_inputs{},
	_outputs{},
	_lineShader(std::weak_ptr<ShaderResource>())
{
	SetNumInputs(numInputs);
	SetNumOutputs(numOutputs);
}

unsigned int GuiRouter::NumInputs() const
{
	return (unsigned int)_inputs.size();
}

unsigned int GuiRouter::NumOutputs() const
{
	return (unsigned int) _outputs.size();
}

void GuiRouter::SetNumInputs(unsigned int num)
{
	if (_inputs.size() == num)
		return;

	if (_inputs.size() > num)
	{
		auto numToRemove = ((unsigned int)_inputs.size()) - num;
		for (auto i = 0u; i < numToRemove; i++)
		{
			auto lastInput = _inputs.back();
			auto lastInputChan = lastInput->Channel();
			_children.erase(std::remove(_children.begin(), _children.end(), lastInput), _children.end());

			// Remove any routes that included this input
			_routes.erase(std::remove_if(_routes.begin(), _routes.end(),
				[lastInputChan](const std::pair<unsigned int, unsigned int>& p) {
					return p.first == lastInputChan;
				}), _routes.end());

			_inputs.pop_back();
		}
	}
	else
	{
		auto numInputs = ((unsigned int)_inputs.size());
		for (auto i = numInputs; i < num; i++)
		{
			auto input = std::make_shared<GuiRouterChannel>(
				_GetChannelParams(
					i,
					true,
					_routerParams.InputType));

			_inputs.push_back(input);
			// Initialize parent links for channels added after construction too.
			AddChild(input);
		}
	}
}

void GuiRouter::SetNumOutputs(unsigned int num)
{
	if (_outputs.size() == num)
		return;

	if (_outputs.size() > num)
	{
		auto numToRemove = ((unsigned int)_outputs.size()) - num;
		for (auto i = 0u; i < numToRemove; i++)
		{
			auto lastOutput = _outputs.back();
			auto lastOutputChan = lastOutput->Channel();
			_children.erase(std::remove(_children.begin(), _children.end(), lastOutput), _children.end());

			// Remove any routes that included this input
			_routes.erase(std::remove_if(_routes.begin(), _routes.end(),
				[lastOutputChan](const std::pair<unsigned int, unsigned int>& p) {
					return p.second == lastOutputChan;
				}), _routes.end());

			_outputs.pop_back();
		}
	}
	else
	{
		auto numOutputs = ((unsigned int)_outputs.size());
		for (auto i = numOutputs; i < num; i++)
		{
			auto output = std::make_shared<GuiRouterChannel>(
				_GetChannelParams(
					i,
					false,
					_routerParams.OutputType));

			_outputs.push_back(output);
			AddChild(output);
		}
	}
}

bool GuiRouter::AddRoute(unsigned int inputChan, unsigned int outputChan)
{
	bool foundRoute = false;

	for (auto& route : _routes)
	{
		if (route.first == inputChan && route.second == outputChan)
		{
			foundRoute = true;
			break;
		}
	}

	if (!foundRoute)
	{
		_routes.push_back(std::make_pair(inputChan, outputChan));
		return true;
	}

	return false;
}

bool GuiRouter::RemoveRoute(unsigned int inputChan, unsigned int outputChan)
{
	bool foundRoute = false;

	for (auto it = _routes.begin(); it != _routes.end();)
	{
		if (it->first == inputChan && it->second == outputChan)
		{
			it = _routes.erase(it);
			foundRoute = true;
		}
		else
		{
			++it;
		}
	}

	return foundRoute;
}

void GuiRouter::ClearRoutes()
{
	_routes.clear();
}

void GuiRouter::SetReceiver(std::weak_ptr<ActionReceiver> receiver)
{
	_routerParams.Receiver = receiver;
}

void GuiRouter::Draw(DrawContext& ctx)
{
	// Scene hover picking can clear child hover after motion routing. Restore
	// proximity feedback for both rows immediately before painting.
	if (_hoverPoint) _UpdateSocketHover(*_hoverPoint);
	for (auto& input : _inputs)
		input->SetActive(std::any_of(_routes.begin(), _routes.end(), [&](const auto& r) { return r.first == input->Channel(); }));
	for (auto& output : _outputs)
		output->SetActive(std::any_of(_routes.begin(), _routes.end(), [&](const auto& r) { return r.second == output->Channel(); }));
	if (_isVisible)
	{
		auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);

		auto pos = Position();
		glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));

		_DrawLines(ctx);

		glCtx.PopMvp();
	}

	GuiElement::Draw(ctx);
}

ActionResult GuiRouter::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	const auto nearby = _NearbyChannel(action.Position, _isDragging && !_isDraggingInput, _isDragging && _isDraggingInput);
	if (nearby)
	{
		if (action.State == TouchAction::TOUCH_DOWN)
			nearby->OnAction(nearby->ParentToLocal(action));
		res.IsEaten = true;
		res.SourceId = ChanToString(nearby->Channel());
		res.ResultType = std::find(_inputs.begin(), _inputs.end(), nearby) != _inputs.end() ? ACTIONRESULT_ROUTERINPUT : ACTIONRESULT_ROUTEROUTPUT;
	}
	if (_isDragging)
	{
		if (TouchAction::TOUCH_UP == action.State)
		{
			_isDragging = false;
			ClearPointerState();
			unsigned int endDragChannel = StringToChan(res.SourceId);

			bool isValid = false;
			bool isEndOutput = res.ResultType == ACTIONRESULT_ROUTEROUTPUT;
			unsigned int inputChan = 0;
			unsigned int outputChan = 0;

			if (_isDraggingInput && isEndOutput)
			{
				isValid = true;
				inputChan = _initDragChannel;
				outputChan = endDragChannel;
			}
			else if (!_isDraggingInput && res.ResultType == ACTIONRESULT_ROUTERINPUT)
			{
				isValid = true;
				inputChan = endDragChannel;
				outputChan = _initDragChannel;
			}

			const bool sameEndpoint = endDragChannel == _initDragChannel &&
				res.ResultType == (_isDraggingInput ? ACTIONRESULT_ROUTERINPUT : ACTIONRESULT_ROUTEROUTPUT);
			if (isValid || sameEndpoint)
			{
				if (isValid)
				{
					if (!AddRoute(inputChan, outputChan))
						RemoveRoute(inputChan, outputChan);
				}
				else
				{
					// Clearing a socket removes every cable attached to that endpoint.
					std::erase_if(_routes, [&](const auto& route) {
						return (_isDraggingInput ? route.first : route.second) == _initDragChannel;
					});
				}

				auto receiver = _routerParams.Receiver.lock();

				if (receiver)
				{
					GuiAction guiAction;
					guiAction.ElementType = GuiAction::ACTIONELEMENT_ROUTER;
					guiAction.Data = GuiAction::GuiConnections(_routes);

					receiver->OnAction(guiAction);
				}

				for (auto i = 0u; i < _inputs.size(); i++)
				{
					bool matchesInput = std::any_of(_routes.begin(), _routes.end(), [i](const auto& pair) {
						return pair.first == i;
						});
					_inputs[i]->SetActive(matchesInput);
				}

				for (auto i = 0u; i < _outputs.size(); i++)
				{
					bool matchesOutput = std::any_of(_routes.begin(), _routes.end(), [i](const auto& pair) {
						return pair.second == i;
						});
					_outputs[i]->SetActive(matchesOutput);
				}

				return {
					true,
					ChanToString(inputChan),
					ChanToString(outputChan),
					ACTIONRESULT_ROUTER,
					nullptr,
					std::static_pointer_cast<base::GuiElement>(shared_from_this())
				};
			}
		}
	}
	else
	{
		if (res.IsEaten && 
			(TouchAction::TOUCH_DOWN == action.State) && 
			((res.ResultType == ACTIONRESULT_ROUTERINPUT) || (res.ResultType == ACTIONRESULT_ROUTEROUTPUT)))
		{
			_isDragging = true;
			_isDraggingInput = ACTIONRESULT_ROUTERINPUT == res.ResultType;
			_initDragChannel = StringToChan(res.SourceId);
			_currentDragPos = action.Position;

			std::cout << "Starting Router drag: " << _initDragChannel << " to (" << _currentDragPos.X << ", " << _currentDragPos.Y << ") " << res.IsEaten << "," << res.ResultType << std::endl;

			return {
				true,
				"",
				"",
				ACTIONRESULT_ACTIVEELEMENT,
				nullptr,
				std::static_pointer_cast<base::GuiElement>(shared_from_this())
			};
		}
	}

	return res;
}

ActionResult GuiRouter::OnAction(TouchMoveAction action)
{
	auto res = GuiElement::OnAction(action);

	if (!_isEnabled || !_isVisible)
		return res;

	res.IsEaten = false;
	res.ResultType = ACTIONRESULT_DEFAULT;

	_hoverPoint = action.Position;
	_UpdateSocketHover(action.Position);

	return res;
}

unsigned int GuiRouter::_GetChannelPos(unsigned int index, unsigned int spacing)
{
	return index * spacing;
}

void GuiRouter::_InitReceivers()
{
	for (auto& input : _inputs)
	{
		input->SetReceiver(ActionReceiver::shared_from_this());
	}

	for (auto& output : _outputs)
	{
		output->SetReceiver(ActionReceiver::shared_from_this());
	}
}

void GuiRouter::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	auto validated = true;

	if (validated)
		validated = _InitShader(resourceLib);
	if (validated)
		validated = _InitVertexArray();

	GlUtils::CheckError("GuiRouter::_InitResources()");
}

void GuiRouter::_ReleaseResources()
{
	graphics::GlDeleteQueue::DeleteBuffers(1, &_vertexBuffer);
	_vertexBuffer = 0;

	graphics::GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;
}

bool GuiRouter::_HitTest(Position2d localPos)
{
	return static_cast<bool>(_NearbyChannel(localPos, false, false));
}

std::shared_ptr<GuiElement> GuiRouter::FindTopmostDescendant(Position2d localPos)
{
	// Keep the router as the pointer owner for both rows, including socket
	// proximity. Decorative labels and sockets must not become hover leaves.
	return IsVisible() && IsEnabled() && _HitTest(localPos) ? shared_from_this() : nullptr;
}

void GuiRouter::ApplyHoverPoint(Position2d localPos)
{
	_hoverPoint = localPos;
	_UpdateSocketHover(localPos);
}

unsigned int GuiRouter::_CalcChannelSpacingY() const
{
	return _routerParams.Size.Height >= _routerParams.InputSize ?
		(_routerParams.Size.Height - _routerParams.InputSize) :
		_routerParams.OutputSize;
}

GuiRouter::GuiRouterChannelParams GuiRouter::_GetChannelParams(unsigned int index,
	bool isInput,
	GuiRouterParams::GuiRouterChannelType type)
{
	auto spacingX = isInput ? _routerParams.InputSpacing : _routerParams.OutputSpacing;
	auto spacingY = _CalcChannelSpacingY();
	auto size = isInput ? _routerParams.InputSize : _routerParams.OutputSize;
	int x = _GetChannelPos(index, spacingX);
	int y = isInput ? spacingY : 0;
	std::string tex = (GuiRouterParams::CHANNEL_DEVICE == type) ?
		_routerParams.DeviceInactiveTexture :
		_routerParams.ChannelInactiveTexture;

	GuiRouterChannelParams chanParams(
		GuiElementParams(
			index,
			DrawableParams{ tex },
			MoveableParams(Position2d{ x, y }, Position3d{ (float)x, (float)y, 0 }, 1.0),
			SizeableParams{ size, size },
			_routerParams.OverTexture,
			_routerParams.DownTexture,
			_routerParams.DownTexture,
			{}),
		index,
		isInput,
		type
	);

	chanParams.TextureShader = "texture_tinted";
	chanParams.TintColor = GuiStyle::Control();
	chanParams.ActiveTexture = (GuiRouterParams::CHANNEL_DEVICE == type) ?
		_routerParams.DeviceActiveTexture :
		_routerParams.ChannelActiveTexture;
	chanParams.HighlightTexture = "rounded_but_on_over";

	return chanParams;
}

bool GuiRouter::_InitShader(ResourceLib& resourceLib)
{
	auto shaderOpt = resourceLib.GetResource(_routerParams.LineShader);

	if (!shaderOpt.has_value())
		return false;

	auto resource = shaderOpt.value().lock();

	if (!resource)
		return false;

	if (SHADER != resource->GetType())
		return false;

	_lineShader = std::dynamic_pointer_cast<ShaderResource>(resource);

	return true;
}

bool GuiRouter::_InitVertexArray()
{
	glGenVertexArrays(1, &_vertexArray);
	glBindVertexArray(_vertexArray);

	glGenBuffers(1, &_vertexBuffer);

	// Max verts = max routes plus 1 for dragging,
	// each route has 2 coords for both start and end (4 in total)
	unsigned int maxVerts = (_MaxRoutes + 1) * 4 * 32;

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, maxVerts * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GlUtils::CheckError("GuiRouter::_InitVertexArray");

	return true;
}

void GuiRouter::_DrawLines(DrawContext& ctx) const
{
	auto shader = _lineShader.lock();

	if (!shader)
		return;

	std::vector<float> vertices;
	float channelSpacingY = (float)_CalcChannelSpacingY();

	for (auto& con : _routes)
	{
		auto inChanPos = (float)_GetChannelPos(con.first, _routerParams.InputSpacing);
		auto outChanPos = (float)_GetChannelPos(con.second, _routerParams.OutputSpacing);
		float x1 = inChanPos + ((float)_routerParams.InputSize * 0.5f);
		float x2 = outChanPos + ((float)_routerParams.OutputSize * 0.5f);
		float y1 = channelSpacingY + (float)_WireYOffset;
		float y2 = (float)_routerParams.OutputSize - (float)_WireYOffset;

		vertices.push_back(x1); vertices.push_back(y1);
		vertices.push_back(x2); vertices.push_back(y2);
	}

	if (_isDragging)
	{
		if (_isDraggingInput)
		{
			auto inChanPos = (float)_GetChannelPos(_initDragChannel, _routerParams.InputSpacing);
			float x = inChanPos + ((float)_routerParams.InputSize * 0.5f);
			float y = channelSpacingY + (float)_WireYOffset;

			vertices.push_back(x); vertices.push_back(y);
			vertices.push_back((float)_currentDragPos.X); vertices.push_back((float)_currentDragPos.Y);
		}
		else
		{
			auto outChanPos = (float)_GetChannelPos(_initDragChannel, _routerParams.OutputSpacing);
			float x = outChanPos + ((float)_routerParams.OutputSize * 0.5f);
			float y = (float)_routerParams.OutputSize - (float)_WireYOffset;

			vertices.push_back(x); vertices.push_back(y);
			vertices.push_back((float)_currentDragPos.X); vertices.push_back((float)_currentDragPos.Y);
		}
	}

	// Cubic curves leave each socket vertically.
	std::vector<float> curves;
	for (size_t i = 0; i + 3 < vertices.size(); i += 4)
	{
		const float x1 = vertices[i], y1 = vertices[i+1], x2 = vertices[i+2], y2 = vertices[i+3];
		const float bend = (y2 - y1) * 0.5f;
		for (int step = 0; step < 32; ++step)
			for (int end = 0; end < 2; ++end)
			{
				const float t = (step + end) / 32.0f, u = 1.0f - t;
				curves.push_back(u*u*u*x1 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x2);
				curves.push_back(u*u*u*y1 + 3*u*u*t*(y1+bend) + 3*u*t*t*(y2-bend) + t*t*t*y2);
			}
	}
	vertices.swap(curves);
	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
	glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(float), vertices.data());

	glLineWidth(6);
	glUseProgram(shader->GetId());

	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
	shader->SetUniforms(glCtx);
	const auto colorUniform = glGetUniformLocation(shader->GetId(), "Color");
	glUniform4f(colorUniform, 1.0f, 0.5f, 0.2f, 0.8f);
	
	glUseProgram(shader->GetId());

	glBindVertexArray(_vertexArray);
	const auto vertexCount = static_cast<GLsizei>(vertices.size() / 2);
	const GLsizei draggedVertices = _isDragging ? 64 : 0;
	glDrawArrays(GL_LINES, 0, vertexCount - draggedVertices);
	if (_isDragging)
	{
		glUniform4f(colorUniform, 0.4f, 0.85f, 1.0f, 1.0f);
		glDrawArrays(GL_LINES, vertexCount - draggedVertices, draggedVertices);
	}
	glBindVertexArray(0);

	glUseProgram(0);
}


void GuiRouter::ClearPointerState()
{
	_hoverPoint.reset();
	_isDragging = false;
	GuiElement::ClearPointerState();
}

void GuiRouter::ApplyHoverState(bool inside)
{
	if (!inside && !_isDragging)
	{
		_hoverPoint.reset();
		for (const auto& channel : _inputs) channel->ApplyHoverState(false);
		for (const auto& channel : _outputs) channel->ApplyHoverState(false);
	}
}
