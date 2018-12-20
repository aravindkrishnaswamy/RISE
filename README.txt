###########################################################

  REALISTIC IMAGE SYNTHESIS ENGINE
    version 1.1.0 Build 80
	May 8, 2006

	(c) 2001-2006 Aravind Krishnaswamy.
	Please see attached LICENSE.TXT file for license 
	information.

	For questions or comments, contact Aravind Krishnaswamy
	at:

	  rise@aravind.ca
	
###########################################################

###########################################################
  TABLE OF CONTENTS

	1. Getting started/FAQ
	2. Setting the media path
	3. What's new?
	4. Known Issues
	5. Usage Notes
	6. Colors

###########################################################

###########################################################
1.  Getting Started/FAQ

  Where you get started using R.I.S.E. will depend 
  heavily on your intended purpose.  These next four
  segments will describe R.I.S.E. a little bit.  The 
  segments following that will attempt to answer where
  to get started depending on your purpose and knowledge.

  I.  What is R.I.S.E. ?
  
    R.I.S.E., which stands for Realistic Image Synthesis
	Engine is a state of the art engine and framework for
	the simulation and realistic image synthesis of 
	natural phenomenon based on physics, biology and
	chemistry.  

  II.  What R.I.S.E. isn't.

    R.I.S.E. is not meant to be a competitor to commercially
	avaiable ray tracing engines such as Brazil, finalRender,
	or v-ray.  It is definitely not meant to be a competitor
	to scanline renderers or micro-polygon tesselating
	engines such as Pixar's RenderMan.  R.I.S.E. is not
	meant to be a tool to be used by a studio to produce
	content for time-critical projects.  It is not meant to
	be an alternative to other excellent open source renderers
	such as Aqsis or Pixie. 

  III.  Who is R.I.S.E. for ?

    R.I.S.E. is primarily intended for researchers 
	involved in the simulation of natural phenomenon.  
	However, artists should be able to use R.I.S.E. to
	generate compelling images as long as they aren't 
	afraid to get their hands a little dirty (and have 
	massive CPU cycles to spare).

	Another primary focus of R.I.S.E. is to provide
	code that is lucid, readible and correct so that 
	people who want to learn the fundamentals of physically-based
 	rendering and ray tracing have something to learn from.

  IV.  What are fundamental principles behind R.I.S.E. ?
  
    There are two main priorities in R.I.S.E.
	  - Physically correct rendering and where applicable
	    biologically correct rendering
	  - General, easy to read and understand code

	The first priority, the physical correctness is 
	achieved by using fundamental principles of Monte
	Carlo methods in the rendering process.  The
	biologically correct rendering is a work in progress.

	The second priority is achieved by sacrificing obvious
	avenues of performance increase.  As much of the code
	remains general with as little specifics as possible.  
	This is also what makes R.I.S.E. able to compile and
	run on virtually any platform with a decent C++
	compiler.

	Both of these principles means that R.I.S.E. will be
	slower than many other available renderers.  However,
	R.I.S.E. should also have an excellent level of
	accuracy since all numerical computation is done with
	double precision floating point.  The engine color
	and spectral pipeline is also double precision floating
	point.

  V.  I am an artist, where do I get started ?

    Run RISE.NET.  Load a sample scene, render
	it out.  The best way to get a feel for the
	scene description language is to take a look through 
	all the sample scenes.  Many of the features in the
	engine are exposed in these scenes.  In addition, most
	of the images in the gallery were generated with
	these scene files.  You should also take a look at 
	the source file Parsers/AsciiSceneParser.cpp in the
	src\Library folder.  Look at the various chunks to
	see the parameters that are available for each of the
	chunks.  Remember that you only need to specify the
	parameters you want different from the default in a
	chunk.

	The R.I.S.E. scene files are configured so that the 
	entire scene can render in a reasonable amount of 
	time on a fast PC.  In order to generate the final, 
	clean, noise-free images, you will have to significantly
	increase the number of samples in these scenes.  In
	scenes with photon mapping, you should also increase
	the number of photons and the number to photons to look
	for on the gather.

  VI.  I am new to raytracing and rendering, and I want
       to write my own, where do I get started?

	If you have Visual Studio .NET, load the RISE.sln
	project.  Build a debug version of the library.  
	Set a break point in Job::Rasterize, step through
	and trace the execution from the RasterizeScene
	function call.  This should give you a feel for how
	the library is organized and how the rendering process
	works.

	If you are using Linux, you can do the same thing in 
	gdb to trace through the execution.

  VII.  I am a researcher and want to use R.I.S.E. in my
        project

	You should probably do the same thing as a newbie and
	trace the flow of execution to get an idea of the 
	library works.  

	Since all of the R.I.S.E. classes are created and 
	exported by the RISE_API.h file, that is where you 
	should begin.  You should be able to create and use
	virtually any element in the library through this 
	interface.

	You should be able to define your own custom classes
	based on the interfaces exported by R.I.S.E. to get
	the library to work with your code.

  VIII.  I am an artist with C++ programming
         experience.  The RISEscene format isn't enough for
	     me.  I want more.  Where do I get started?

   	Your best bet is to use the Job interface to create custom
	scenes in C++ code.  This way you have the full
	flexibility and programmability of the C++ language
	and all the features of R.I.S.E.  In fact the scene
	parser is just built on top of the Job interface. 

	Take a look at IJob.h in the Interfaces folder to get
	an idea of what the interface looks like.

	If you want to use C#, you also have the option of using
	the ManagedJob interface, which is exported by the
	ManagedJob.dll Assembly.  However be warned that several
	Job interface methods are missing in the ManagedJob.  
  
###########################################################

###########################################################
2.  SETTING THE MEDIA PATH

You must set the RISE_MEDIA_PATH environment variable
to point to the base path of the all media for your
scene.

You should also set the RISE_OPTION_FILE environment
variable to point to a file that contains options.  By 
default R.I.S.E. will load a file named 'global.options'
in either the launch path or from one of the media
paths.

If you ran the R.I.S.E. installer for Windows, it will have
already set your media path to the folder you installed
R.I.S.E. to.  If you want to change the media path or if
your settings are corrupted, follow these steps:

On Windows:

Start->Control Panel
Double click on System
Click on the 'Advanced' tab
Click on 'Environment Variables'
In the 'System variables' window look for 'RISE_MEDIA_PATH'
If you double click on it, you can change the path

If you do not see the 'RISE_MEDIA_PATH' variable, click on 
'New' and create on with that name and set the path to the 
root folder of all your media:

i.e. "C:\Program files\Realistic Image Synthesis Engine\"

On Linux:

Depending on your shell type, set the media path to the right
location.  

i.e. in bash:

RISE_MEDIA_PATH=/home/aravind/rise/;export RISE_MEDIA_PATH

You can also set and remove media paths from within the 
R.I.S.E. command console or from which in a RISEScript or
RISEScene file.

There are three commands dealing with the media path, 'add',
'remove' and 'clearall'.  Each does as their names suggest.
It should be noted that media location substitution is 
done in the order in which the media paths are set.

From within the command console:

mediapath add <new path>
mediapath remove <existing path>
       ...

From within a RISEScene file:

> media path add <new path>
       ...

###########################################################

###########################################################
3.  WHAT'S NEW

- Options file has been added for controlling various global rendering parameters
- Added OpenEXR support (half format only)
- Made irradiance caching two pass
- Improved orthographic camera
- Trimmed ManagedJob to just the parts that we need for .NET version
- Fixed negative value bug with Radiance HDR format
- Improved SSS by doing a proper sampling of mesh geometry (triangle mesh geometry now randomly picks the triangle according to the CDF of the areas)
- Thread safe final gather
- Thread safe rasterizer state caching
- Number of CPUs are now counted (windows only)
- Updated external libraries (libpng, zlib, libtiff)
- MP support (windows only)
- Proper color management
- Internal color space is now linear ROMM RGB (ProPhotoRGB)
- XCode 2.2 is now the supported version
- Visual 2005 project generation
- Fixed bug in CSG Object intersection_only
- Fixed non updates of UI in .NET
- Copyright changes
- Fix for the zr / zv stuff in diffusion approximation SSS
- Using a combination and cubic spline interpolation and linear interpolator in the DataDrivenBSDF
- Fixes bugs related to random numbers (from Danilo)
- Fixed bug with BSP and Octrees and ignoring hidden objects
- Shadow ray distances are pulled back by 0.01
- Fixed bug with not dividing by num samples with 0 variance after taking initial samples
- ProgressCall refactoring
- Atmopsherics (3DS MAX only)
- Moved the GlobalRadianceMap to the scene from the RayCaster
- Added environments to the 3DS MAX plugin
- Moved RASTERIZER_STATE into the RayIntersectionGeometric
- Pass the RASTERIZER_STATE into BSDFs and consequently lights as well
- Added tooltips

###########################################################

###########################################################
4.  Known Issues

 - On certain machines, sometimes, at random the 
   scene parser will throw an error saying a chunk could not
   be parsed, with invalid data as the chunk name.  We believe
   this is some bug within the scene parser and the istream 
   input.  This will hopefully be resolved in a future service
   release.  If you run into this, your only choice is shutdown
   the application and start again, or just try loading the 
   scene file again.

 - RISE.NET on machines with multiple processors when rendering
   with more than one processor will not respond properly to 
   'Pause Rendering' and 'Cancel Rendering' commands.  In fact
   'Cancel Rendering' will cause the UI to freeze.

 - RISE.NET.exe and RISE.exe for the Windows platform are
   for the Pentium 4 or higher (or AMD Opteron) only.
   Running these files on other processors will cause
   a crash with illegal instruction.  Pentium 3 builds 
   can be made available upon request.  Please contact us.

 - Writing image files in any colorspace other than ProPhotoRGB
   or ROMMRGB_Linear are not guaranteed to be accurate.  This
   is because color spaces like Rec709 RGB (on which sRGB is
   based) have a smaller gamut, hence some form of gamut
   mapping must be performed.  Gamut mapping is a tricky 
   process often involving some user intervention, hence
   the algorithm R.I.S.E. uses is very simple and may not
   produce visually pleasing results.  What you want to do
   is to write your images as ProPhotoRGB, then open up the
   image in Adobe Photoshop, Assign the ProPhotoRGB profile
   to the file, then do a "Convert to Profile" to convert
   it to a suitable profile/color space.  This is the best
   way to get the highest quality results.

 - With RISE.NET.exe, scenes with photon mapping take too
   long to load.
     > The reason they take long is because the photon map
	   is computed on load, however there is no indication
	   on the UI that this is happening (there is an indic-
	   ation in the command prompt and Linux versions)
	 > The log window will update during loads so you can
	   see what the engine is doing

 - DRISE requires ALL the scene resources to be locally
   present.  The only thing that is communicated from the 
   server to the client is the name of the scene file.

 - Using Halton point sequence sampling with DRISE may cause
   aliasing and artifacts in your results.

 - For those using RISE_API.h with MFC applications, if you
   don't include RISE_API.h BEFORE your MFC includes, you
   will get compilation errors.  It is believed this is
   because the MFC headers are defining something critical
   that R.I.S.E. is using

 - There are quirks when using nested CSG objects

 - Using low sample counts with some of the pixel filters
   causes lots of noise, especially the windowed sinc
   filters because of their negative lobes
  
 - Using predict on scenes with irradiance caching will
   cause inaccurate predictions and will cause the
   prediction to take a really long time (no a REALLY long
   time)

 - Using predict with the PixelPelAdaptiveSamplingRasterizer
   will cause inaccruate predictions.  The returned value
   is the minimum amount of time only

 - Using predict with any scenes containing a subsurface
   scattering shaderop will cause inaccurate prediction.
   This is because the precomputation time to generate
   the point approximation is included in the prediction.
   In general, the real time to render scene will be less
   than the predicted time.

 - Using the PixelPelAdaptiveSamplingRasterizer with the
   minimum sample count too low will cause errors, try
   to use at least about 8-16 samples.  If you are using
   the path tracer or one of the windowed sinc filters
   then you will want even more samples (at least 256)

 - The macro substitution features in the scene parser
   are still experimental, so improper use may result in 
   wierd or cryptic errors
 
 - The looping features in the scene parser are still
   experimental, so improper loop specification may result
   in wierd or cryptic errors

 - Some methods defined in IJob.h are not available in 
   ManagedJob.

 - Source code to the BioSpec model is not included, but
   it is available in the compiled versions.
  
###########################################################

###########################################################
5.  Usage Notes

  A. Linux or Command Prompt
    - rise <scene file>
	   Loads the given scene file
	- rise -pr
	   Computes the 'performance rating' (a unitless number
	   quantifying the performance of the computer for
	   R.I.S.E. operations.  For relative comparisons only)
	- rise -highperf
	   Computes a more thorough performance rating, takes 
	   about 5 times longer than -pr

	- Commands
	   > predict - predicts the time to render the current
	               scene
	   > render - renders the current scene
	   > render <left> <right> <top> <bottom> - renders the
	       given rectangle
	   > render (blocks|hilbert|scanlines) <param> - renders
	       using the given rendering order
	   > renderanimation - renders an animation sequence
	       using the animations options in the scene
	   > clearall - clears everything loaded, completely
	       empty scene is set
	   > load <scene file> - Loads the given scene file

  B. RISE.NET
    - Launch
	- Use Load Scene to load a scene
	- Pause will pause the rendering and resume will
	  resume the rendering thread

  C. meshconverter.exe
    - Use to convert 3DS and PLY meshes to .RISEmesh format
	- Automatically detects the file extension
	- Can specify the use of BSP or Octrees and whether to
	  use face normals or generate vertex normals

  D. imageconverter.exe
    - Use to convert images between various types
	- Automatically detects by file extension
	- Can specify resizing parameters

  E. DRISE_Server.exe
    - Run the server on a machine
	- No UI will appear
	- Port is set in the drise_options file

  F. DRISE_SimpleJobSubmitter.exe
    - Use to the submit jobs
	- Run with no parameters for usage instructions

  G. DRISE_Client.exe
    - Run the client
	- No UI will appear
	- Port and server settings are in the drise_options file
	- Remember that all scene resources must be locally
	  present.
  
###########################################################

###########################################################

6.  Colors

R.I.S.E. uses the ROMM RGB (also known as ProPhotoRGB)
as its internal color space.  The reason for this is because
ROMM RGB has a significantly larger gamut than either
Rec709 RGB (on which sRGB is based) or AdobeRGB.  In order
to ensure accurate color reproduction you should 
specify your color spaces for input textures.  

WARNING!  Most external viewers are not color profile 
aware.  What you want to do is to write your images as 
ProPhotoRGB, then open up the image in Adobe Photoshop, 
Assign the ProPhotoRGB profile to the file, then do a 
"Convert to Profile" to convert it to a suitable 
profile/color space.  This is the best way to get the 
highest quality results.

So why ProPhotoRGB?  Most printers have a gamut larger
than sRGB, hence for the highest quality color reproduction
you want your renderer to do its operations in a large gamut
color space at high precision (which is exactly what 
R.I.S.E. does).  

However converting from a color space with a large gamut
to a smaller one is a tricky process.  It often involves
some form of user intervention (depending on the rendering
intent).  Hence you will want to use some program (like
Adobe Photoshop) to do your conversions for you.  Also
you will want to use an application like Adobe Photoshop
to do further editing, as most other applications are not
color profile/space aware.

###########################################################
