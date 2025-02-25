#include "GuiRouter.h"

using namespace base;
using namespace gui;
using namespace utils;
using namespace actions;
using namespace resources;

const unsigned int GuiRouter::_ChannelGapX = 8;
const unsigned int GuiRouter::_ChannelGapY = 64;
const unsigned int GuiRouter::_ChannelWidth = 30;
const unsigned int GuiRouter::_MaxRoutes = 128;
const int GuiRouter::_WireYOffset = 6;

GuiRouter::GuiRouterChannel::GuiRouterChannel(GuiRouterChannelParams params) :
	GuiElement(params),
	_isActive(false),
	_activeTexture(graphics::ImageParams(DrawableParams{ params.ActiveTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_highlightTexture(graphics::ImageParams(DrawableParams{ params.HighlightTexture }, SizeableParams{ params.Size,params.MinSize }, "texture")),
	_label(nullptr),
	_params(params)
{
	GuiLabelParams labelParams(GuiElementParams(0,
		DrawableParams{ "" },
		MoveableParams(Position2d{ 2, 2 }, Position3d{ 2, 2, 0 }, 1.0),
		SizeableParams{ params.Size.Width - 4, params.Size.Height - 4 },
		"",
		"",
		"",
		{}), std::to_string(params.Channel));

	_label = std::make_unique<GuiLabel>(labelParams);
}

void GuiRouter::GuiRouterChannel::Draw(base::DrawContext& ctx)
{
	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);

	auto pos = Position();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0), glm::vec3(pos.X, pos.Y, 0.f)));

	if (STATE_DOWN == _state)
		_downTexture.Draw(ctx);
	else
	{
		if (STATE_OVER == _state)
			_overTexture.Draw(ctx);

		if (_isActive)
			_activeTexture.Draw(ctx);
		else
			_texture.Draw(ctx);
	}

	for (auto& child : _children)
		child->Draw(ctx);

	glCtx.PopMvp();
}

ActionResult GuiRouter::GuiRouterChannel::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten)
	{
		res.SourceId = ChanToString(_params.Channel);
		res.ResultType = _params.IsInput ?
			ACTIONRESULT_ROUTERINPUT :
			ACTIONRESULT_ROUTEROUTPUT;
	}

	return res;
}

ActionResult GuiRouter::GuiRouterChannel::OnAction(TouchMoveAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten)
		return res;

	/*res.IsEaten = false;
	res.ResultType = ACTIONRESULT_DEFAULT;

	if (!_isDragging)
		return res;

	_currentDragPos = action.Position;*/

	return res;
}

void GuiRouter::GuiRouterChannel::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	GuiElement::_InitResources(resourceLib, forceInit);

	_activeTexture.InitResources(resourceLib, forceInit);
	_highlightTexture.InitResources(resourceLib, forceInit);

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
	unsigned int numOutputs,
	bool isInputDevice,
	bool isOutputDevice) :
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
	auto createGuiRouterChannelParams = [params](int index, bool isInput, bool isDevice) {
		int x = _GetChannelPos(index);
		int y = isInput ? _ChannelWidth + _ChannelGapY : 0;
		std::string tex = isDevice ? params.DeviceInactiveTexture : params.ChannelInactiveTexture;

		GuiRouterChannelParams chanParams(
			GuiElementParams(
				index,
				DrawableParams{ tex },
				MoveableParams(Position2d{ x, y }, Position3d{ (float)x, (float)y, 0 }, 1.0),
				SizeableParams{ _ChannelWidth, _ChannelWidth },
				params.OverTexture,
				params.DownTexture,
				params.DownTexture,
				{}),
			index,
			isInput,
			isDevice
		);

		chanParams.ActiveTexture = isDevice ? params.DeviceActiveTexture : params.ChannelActiveTexture;
		chanParams.HighlightTexture = params.HighlightTexture;

		return chanParams;
	};

	for (unsigned int i = 0; i < numInputs; i++)
	{
		auto input = std::make_shared<GuiRouterChannel>(
			createGuiRouterChannelParams(
				i,
				true,
				isInputDevice));

		_inputs.push_back(input);
		_children.push_back(input);
	}

	for (unsigned int i = 0; i < numOutputs; i++)
	{
		auto output = std::make_shared<GuiRouterChannel>(
			createGuiRouterChannelParams(
				i,
				false,
				isOutputDevice));

		_outputs.push_back(output);
		_children.push_back(output);
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
	_DrawLines(ctx);

	GuiElement::Draw(ctx);
}

ActionResult GuiRouter::OnAction(TouchAction action)
{
	auto res = GuiElement::OnAction(action);

	if (_isDragging)
	{
		if (TouchAction::TOUCH_UP == action.State)
		{
			_isDragging = false;
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
			else if (!_isDraggingInput && !isEndOutput)
			{
				isValid = true;
				inputChan = endDragChannel;
				outputChan = _initDragChannel;
			}

			if (isValid)
			{
				std::cout << "Finished Router drag: " << inputChan << " to " << outputChan << std::endl;

				if (!AddRoute(inputChan, outputChan))
					RemoveRoute(inputChan, outputChan);

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
		if (res.IsEaten && (TouchAction::TOUCH_DOWN == action.State))
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

	return GuiElement::OnAction(action);
}

ActionResult GuiRouter::OnAction(TouchMoveAction action)
{
	auto res = GuiElement::OnAction(action);

	if (res.IsEaten)
		return res;

	res.IsEaten = false;
	res.ResultType = ACTIONRESULT_DEFAULT;

	if (!_isDragging)
		return res;

	_currentDragPos = action.Position;

	return res;
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
	glDeleteBuffers(1, &_vertexBuffer);
	_vertexBuffer = 0;

	glDeleteVertexArrays(1, &_vertexArray);
	_vertexArray = 0;
}

bool GuiRouter::_HitTest(Position2d localPos)
{
	return false;
}

const unsigned int GuiRouter::_GetChannelPos(unsigned int index)
{
	return (index * _ChannelWidth) + ((index + 1) * _ChannelGapX);
}

const unsigned int GuiRouter::_GetChannel(unsigned int pos)
{
	return (pos - _ChannelGapX) / (_ChannelWidth + _ChannelGapX);
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
	unsigned int maxVerts = (_MaxRoutes + 1) * 4;

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

	for (auto& con : _routes)
	{
		auto inChanPos = (float)_GetChannelPos(con.first);
		auto outChanPos = (float)_GetChannelPos(con.second);
		float x1 = inChanPos + ((float)_ChannelWidth * 0.5f) + _ChannelGapX;
		float x2 = outChanPos + ((float)_ChannelWidth * 0.5f) + _ChannelGapX;
		float y1 = (float)(_ChannelGapY + _ChannelWidth) + (float)_WireYOffset;
		float y2 = (float)_ChannelWidth - (float)_WireYOffset;

		vertices.push_back(x1); vertices.push_back(y1);
		vertices.push_back(x2); vertices.push_back(y2);
	}

	if (_isDragging)
	{
		if (_isDraggingInput)
		{
			auto inChanPos = (float)_GetChannelPos(_initDragChannel);
			float x = inChanPos + ((float)_ChannelWidth * 0.5f) + _ChannelGapX;
			float y = (float)(_ChannelGapY + _ChannelWidth) + (float)_WireYOffset;

			vertices.push_back(x); vertices.push_back(y);
			vertices.push_back((float)_currentDragPos.X); vertices.push_back((float)_currentDragPos.Y);
		}
		else
		{
			auto outChanPos = (float)_GetChannelPos(_initDragChannel);
			float x = outChanPos + ((float)_ChannelWidth * 0.5f) + _ChannelGapX;
			float y = (float)_ChannelWidth - (float)_WireYOffset;

			vertices.push_back(x); vertices.push_back(y);
			vertices.push_back((float)_currentDragPos.X); vertices.push_back((float)_currentDragPos.Y);
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, _vertexBuffer);
	glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(float), vertices.data());

	glLineWidth(6);
	glUseProgram(shader->GetId());

	auto& glCtx = dynamic_cast<graphics::GlDrawContext&>(ctx);
	glCtx.SetUniform("color", glm::vec4(1.0f, 0.5f, 0.2f, 0.8f));
	shader->SetUniforms(dynamic_cast<graphics::GlDrawContext&>(ctx));
	
	glUseProgram(shader->GetId());

	glBindVertexArray(_vertexArray);
	glDrawArrays(GL_LINES, 0, (unsigned int)(vertices.size() / 2));
	glBindVertexArray(0);

	glUseProgram(0);
}
