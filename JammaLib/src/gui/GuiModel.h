#pragma once

#include "GuiElement.h"
#include "GlUtils.h"

namespace gui
{
	class GuiModelParams : public base::GuiElementParams
	{
	public:
		GuiModelParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			ModelTextures({}),
			ModelShaders({}),
			Verts({}),
			Uvs({})
		{
		}

		GuiModelParams(base::GuiElementParams params) :
			base::GuiElementParams(params),
			ModelTextures({}),
			ModelShaders({}),
			Verts({}),
			Uvs({})
		{
		}

	public:
		std::vector<std::string> ModelTextures;
		std::vector<std::string> ModelShaders;
		std::vector<float> Verts;
		std::vector<float> Uvs;
	};

	class GuiModel :
		public base::GuiElement
	{
	public:
		GuiModel(GuiModelParams params);

	public:
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		void SetGeometry(std::vector<float> coords, std::vector<float> uvs);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		bool InitTextures(resources::ResourceLib& resourceLib);
		bool InitShaders(resources::ResourceLib& resourceLib);
		bool InitVertexArray(std::vector<float> verts, std::vector<float> uvs);
		virtual std::weak_ptr<resources::TextureResource> GetTexture()
		{
			if (!_modelTextures.empty())
				return *_modelTextures.begin();

			return std::weak_ptr<resources::TextureResource>();
		}
		virtual std::weak_ptr<resources::ShaderResource> GetShader()
		{
			if (!_modelShaders.empty())
				return *_modelShaders.begin();

			return std::weak_ptr<resources::ShaderResource>();
		}

	protected:
		bool _resourcesInitialised;
		bool _geometryNeedsUpdating;
		GuiModelParams _modelParams;
		std::vector<float> _backVerts;
		std::vector<float> _backUvs;
		GLuint _vertexArray;
		GLuint _vertexBuffer[3];
		unsigned int _numTris;
		std::vector<std::weak_ptr<resources::TextureResource>> _modelTextures;
		std::vector<std::weak_ptr<resources::ShaderResource>> _modelShaders;
	};
}
