#pragma once

#include "GuiElement.h"
#include "GlUtils.h"

namespace gui
{
	class GuiModelParams : public base::GuiElementParams
	{
	public:
		GuiModelParams() :
			base::GuiElementParams(DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			ModelTexture(""),
			ModelShader(""),
			Verts({}),
			Uvs({})
		{
		}

		GuiModelParams(base::GuiElementParams params) :
			base::GuiElementParams(params),
			ModelTexture(""),
			ModelShader(""),
			Verts({}),
			Uvs({})
		{
		}

	public:
		std::string ModelTexture;
		std::string ModelShader;
		std::vector<float> Verts;
		std::vector<float> Uvs;
	};

	class GuiModel :
		public base::GuiElement
	{
	public:
		GuiModel(GuiModelParams params);

	public:
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances) override;

		void SetGeometry(std::vector<float> coords, std::vector<float> uvs);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		bool InitTexture(resources::ResourceLib& resourceLib);
		bool InitShader(resources::ResourceLib& resourceLib);
		bool InitVertexArray(std::vector<float> verts, std::vector<float> uvs);

	protected:
		bool _resourcesInitialised;
		bool _geometryNeedsUpdating;
		GuiModelParams _modelParams;
		std::vector<float> _backVerts;
		std::vector<float> _backUvs;
		GLuint _vertexArray;
		GLuint _vertexBuffer[3];
		unsigned int _numTris;
		std::weak_ptr<resources::TextureResource> _modelTexture;
		std::weak_ptr<resources::ShaderResource> _modelShader;
	};
}
