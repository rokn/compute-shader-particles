#include "config.h"
#include "shaders.h"

#include <iostream>
using namespace std;

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>

static GLFWwindow *window = nullptr;
static int frames = 0;

static Config config;

static string vertexShaderCode = "\
#version 430\n\
out vec2 uv;\n\
in vec2 pos;\n\
void main() {\n\
	gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);\n\
	uv = 0.5 * pos + 0.5;\n\
}";

static string fragmentShaderCode = "\
#version 430\n\
in vec2 uv;\n\
uniform sampler2D sampler;\n\
out vec3 color;\n\
void main() {\n\
	color = texture(sampler, uv).rgb;\n\
}";

static string renderTextureComputeShaderCode = "\
#version 430\n\
layout(binding = 0, rgba32f) uniform writeonly image2D outputTexture;\n\
layout(binding = 1, r32ui) uniform uimage2D particleCountTexture;\n\
layout(local_size_x = 16, local_size_y = 16) in;\n\
float w = ${width}, h = ${height};\n\
void main() {\n\
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);\n\
	uint v = imageAtomicExchange(particleCountTexture, pos, 0);\n\
	imageStore(outputTexture, pos, vec4(v / 10.0, 0.0, 0.0, 1.0));\n\
}";

static string updateParticlesComputeShaderCode = "\
#version 430\n\
layout(binding = 1, r32ui) uniform uimage2D particleCountTexture;\n\
layout(binding = 2, rgba32f) uniform image2D particlePositionTexture;\n\
layout(local_size_x = 16, local_size_y = 16) in;\n\
float w = ${width}, h = ${height};\n\
void main() {\n\
	ivec2 id = ivec2(gl_GlobalInvocationID.xy);\n\
	vec4 position = imageLoad(particlePositionTexture, id);\n\
	imageAtomicAdd(particleCountTexture, ivec2(position.x, position.y), 1);\n\
}";

string formatShaderCode(string shaderCode)
{
	for (auto p : {pair<string, string>("width", to_string(config.width)),
				   {"height", to_string(config.height)}}) {
		string s = "${" + p.first + "}";
		for (;;) {
			auto i = shaderCode.find(s);
			if (i == string::npos)
				break;
			shaderCode.replace(i, s.size(), p.second);
		}
	}
	return shaderCode;
}


static void checkForErrors(string where)
{
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		cerr << "OpenGL error in " << where << ": " <<  gluErrorString(e) << " (" << e << ")" << endl;
		exit(1);
	}
}

int main()
{
	// Initialize GLFW
	if (!glfwInit()) {
		cerr << "Failed to initialize GLFW" << endl;
		return 1;
	}

	// Create window
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	window = glfwCreateWindow(config.width, config.height, WINDOW_TITLE, nullptr, nullptr);
	if (window == nullptr) {
		cerr << "Failed to open window" << endl;
		return 1;
	}

	glfwMakeContextCurrent(window);

	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK) {
		cerr << "Failed to initialize GLEW" << endl;
		return 1;
	}


	// Create the main texture
	GLuint textureID;
	{
		auto sz = config.width * config.height;
		float *data = new float[sz * 4];
		for (int i = 0; i < sz; ++i) {
			float c = float(i) / sz;
			data[i * 4 + 0] = c;
			data[i * 4 + 1] = c;
			data[i * 4 + 2] = c;
			data[i * 4 + 3] = 1.0;
		}

		glGenTextures(1, &textureID);
		glBindTexture(GL_TEXTURE_2D, textureID);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, config.width, config.height, 0, GL_RGBA, GL_FLOAT, data);
		delete[] data;
		checkForErrors("Generate texture");
	}

	// Create a integer texture, where every pixel's value is the number of particles
	// that should be positioned on the corresponding pixel on the main texture/screen
	GLuint particleCountTextureID;
	{
		auto sz = config.width * config.height * 4;
		GLuint *data = new GLuint[sz];
		for (int i = 0; i < sz; ++i)
			data[i] = 2;
		glGenTextures(1, &particleCountTextureID);
		glBindTexture(GL_TEXTURE_2D, particleCountTextureID);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, config.width, config.height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, data);
		delete[] data;
		checkForErrors("Generate particle count texture");
	}

	// Create a texture in which the particle coordinates will be stored
	GLuint particlePositionTextureID;
	{
		int sizeX = config.particleCountX;
		int sizeY = config.particleCountY;

		auto sz = sizeX * sizeY;
		float *data = new float[sz * 4];
		for (int i = 0; i < sz; ++i) {
			data[i * 4 + 0] = config.width * (rand() / float(RAND_MAX));
			data[i * 4 + 1] = config.height * (rand() / float(RAND_MAX));
			data[i * 4 + 2] = 0.0;
			data[i * 4 + 3] = 0.0;
		}

		glGenTextures(1, &particlePositionTextureID);
		glBindTexture(GL_TEXTURE_2D, particlePositionTextureID);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, sizeX, sizeY, 0, GL_RGBA, GL_FLOAT, data);
		delete[] data;
		checkForErrors("Generate particle position texture");
	}

	// Create the render program
	GLuint vertexID = createShader(vertexShaderCode, GL_VERTEX_SHADER, "Vertex shader");
	GLuint fragmentID = createShader(fragmentShaderCode, GL_FRAGMENT_SHADER, "Fragment shader");
	GLuint renderProgramID = linkProgram({vertexID, fragmentID});

	// Create the shader programs
	GLuint renderTexureProgramID = linkProgram({createShader(formatShaderCode(renderTextureComputeShaderCode), GL_COMPUTE_SHADER, "Render texture compute shader")});
	GLuint updateParticlesProgramID = linkProgram({createShader(formatShaderCode(updateParticlesComputeShaderCode), GL_COMPUTE_SHADER, "Update particles compute shader")});

	// Two triangles
	GLuint vertArray;
	glGenVertexArrays(1, &vertArray);
	glBindVertexArray(vertArray);

	GLuint posBuf;
	glGenBuffers(1, &posBuf);
	glBindBuffer(GL_ARRAY_BUFFER, posBuf);
	float data[] = {
		-1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f, -1.0f,
		 1.0f,  1.0f
	};
	glBufferData(GL_ARRAY_BUFFER, sizeof(float)*8, data, GL_STREAM_DRAW);
	GLint posPtr = glGetAttribLocation(renderProgramID, "pos");
	glVertexAttribPointer(posPtr, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(posPtr);

	glClearColor(0.0, 0.0, 0.0, 1.0);

	glfwSwapInterval(config.enableVSync);
	glfwSetTime(0.0);
	while (!glfwWindowShouldClose(window)) {
		glClear(GL_COLOR_BUFFER_BIT);

		glActiveTexture(GL_TEXTURE0);
		glBindImageTexture(0, textureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindImageTexture(1, particleCountTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
		glActiveTexture(GL_TEXTURE0 + 2);
		glBindImageTexture(2, particlePositionTextureID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

		// Update the particles
		glUseProgram(updateParticlesProgramID);
		glUniform1i(glGetUniformLocation(updateParticlesProgramID, "particleCountTexture"), 1);
		glUniform1i(glGetUniformLocation(updateParticlesProgramID, "particlePositionTexture"), 2);
		glDispatchCompute(config.particleCountX / 16, config.particleCountY / 16, 1);
		checkForErrors("Dispatch update particles compute shader");

		// Render the output texture
		glUseProgram(renderTexureProgramID);
		glUniform1i(glGetUniformLocation(updateParticlesProgramID, "outputTexture"), 0);
		glUniform1i(glGetUniformLocation(renderTexureProgramID, "particleCountTexture"), 1);
		glDispatchCompute(config.width / 16, config.height / 16, 1);
		checkForErrors("Dispatch render texture compute shader");

		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		glUseProgram(renderProgramID);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textureID);
		glUniform1i(glGetUniformLocation(renderProgramID, "sampler"), 0);

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		glfwSwapBuffers(window);
		glfwPollEvents();

		++frames;
		double time = glfwGetTime();
		if (time >= 1.0) {
			glfwSetTime(0.0);
			cerr << "FPS: " << frames / time << endl;
			frames = 0;
		}
	}

	return 0;
}
