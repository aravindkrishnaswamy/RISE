using System;
using System.Drawing;
using System.Collections;
using System.ComponentModel;
using System.Windows.Forms;
using System.Data;
using System.Threading;


namespace RISETest
{
	/// <summary>
	/// Summary description for Form1.
	/// </summary>
	public class Form1 : System.Windows.Forms.Form
	{
		private System.ComponentModel.IContainer components;
		private System.Windows.Forms.Button buttonRender;
		private System.Windows.Forms.Button buttonLoadScene;
		private System.Windows.Forms.Button buttonPredict;

		private RISE.Job job = null;
		private string sceneFile = null;
		private System.Windows.Forms.Button buttonPauseRendering;
		private System.Windows.Forms.Button buttonStopRendering;
		private System.Windows.Forms.Button buttonRenderAnimation;
		private System.Windows.Forms.PictureBox pictureBox1;
		private System.Windows.Forms.ToolTip toolTip1;
		private Thread renderThread = null;
		
		public Form1()
		{
			//
			// Required for Windows Form Designer support
			//
			InitializeComponent();

			//
			// TODO: Add any constructor code after InitializeComponent call
			//
		}

		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		protected override void Dispose( bool disposing )
		{
			if( disposing )
			{
				if (components != null) 
				{
					components.Dispose();
				}
			}
			base.Dispose( disposing );
		}

		#region Windows Form Designer generated code
		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		private void InitializeComponent()
		{
			this.components = new System.ComponentModel.Container();
			System.Resources.ResourceManager resources = new System.Resources.ResourceManager(typeof(Form1));
			this.buttonRender = new System.Windows.Forms.Button();
			this.buttonLoadScene = new System.Windows.Forms.Button();
			this.buttonPredict = new System.Windows.Forms.Button();
			this.buttonPauseRendering = new System.Windows.Forms.Button();
			this.buttonStopRendering = new System.Windows.Forms.Button();
			this.buttonRenderAnimation = new System.Windows.Forms.Button();
			this.pictureBox1 = new System.Windows.Forms.PictureBox();
			this.toolTip1 = new System.Windows.Forms.ToolTip(this.components);
			this.SuspendLayout();
			// 
			// buttonRender
			// 
			this.buttonRender.BackColor = System.Drawing.Color.DimGray;
			this.buttonRender.Enabled = false;
			this.buttonRender.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
			this.buttonRender.ForeColor = System.Drawing.Color.LightSteelBlue;
			this.buttonRender.Location = new System.Drawing.Point(176, 80);
			this.buttonRender.Name = "buttonRender";
			this.buttonRender.Size = new System.Drawing.Size(88, 40);
			this.buttonRender.TabIndex = 2;
			this.buttonRender.Text = "Render!";
			this.toolTip1.SetToolTip(this.buttonRender, "Render the current scene");
			this.buttonRender.Click += new System.EventHandler(this.buttonRender_Click);
			// 
			// buttonLoadScene
			// 
			this.buttonLoadScene.BackColor = System.Drawing.Color.DimGray;
			this.buttonLoadScene.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
			this.buttonLoadScene.ForeColor = System.Drawing.Color.LightSteelBlue;
			this.buttonLoadScene.Location = new System.Drawing.Point(0, 80);
			this.buttonLoadScene.Name = "buttonLoadScene";
			this.buttonLoadScene.Size = new System.Drawing.Size(88, 40);
			this.buttonLoadScene.TabIndex = 0;
			this.buttonLoadScene.Text = "Load Scene";
			this.toolTip1.SetToolTip(this.buttonLoadScene, "Load a scene to render");
			this.buttonLoadScene.Click += new System.EventHandler(this.buttonLoadScene_Click);
			// 
			// buttonPredict
			// 
			this.buttonPredict.BackColor = System.Drawing.Color.DimGray;
			this.buttonPredict.Enabled = false;
			this.buttonPredict.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
			this.buttonPredict.ForeColor = System.Drawing.Color.LightSteelBlue;
			this.buttonPredict.Location = new System.Drawing.Point(88, 80);
			this.buttonPredict.Name = "buttonPredict";
			this.buttonPredict.Size = new System.Drawing.Size(88, 40);
			this.buttonPredict.TabIndex = 1;
			this.buttonPredict.Text = "Predict";
			this.toolTip1.SetToolTip(this.buttonPredict, "Predict the amount of time it will take to render this scene");
			this.buttonPredict.Click += new System.EventHandler(this.buttonPredict_Click);
			// 
			// buttonPauseRendering
			// 
			this.buttonPauseRendering.BackColor = System.Drawing.Color.DimGray;
			this.buttonPauseRendering.Enabled = false;
			this.buttonPauseRendering.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
			this.buttonPauseRendering.ForeColor = System.Drawing.Color.LightSteelBlue;
			this.buttonPauseRendering.Location = new System.Drawing.Point(0, 120);
			this.buttonPauseRendering.Name = "buttonPauseRendering";
			this.buttonPauseRendering.Size = new System.Drawing.Size(176, 24);
			this.buttonPauseRendering.TabIndex = 3;
			this.buttonPauseRendering.TabStop = false;
			this.buttonPauseRendering.Text = "Pause Rendering";
			this.toolTip1.SetToolTip(this.buttonPauseRendering, "Pause the current rendering");
			this.buttonPauseRendering.Click += new System.EventHandler(this.buttonPauseRendering_Click);
			// 
			// buttonStopRendering
			// 
			this.buttonStopRendering.BackColor = System.Drawing.Color.DimGray;
			this.buttonStopRendering.Enabled = false;
			this.buttonStopRendering.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
			this.buttonStopRendering.ForeColor = System.Drawing.Color.LightSteelBlue;
			this.buttonStopRendering.Location = new System.Drawing.Point(176, 120);
			this.buttonStopRendering.Name = "buttonStopRendering";
			this.buttonStopRendering.Size = new System.Drawing.Size(176, 24);
			this.buttonStopRendering.TabIndex = 4;
			this.buttonStopRendering.TabStop = false;
			this.buttonStopRendering.Text = "Stop Rendering";
			this.toolTip1.SetToolTip(this.buttonStopRendering, "Stop the current rendering (you will not be able to restart)");
			this.buttonStopRendering.Click += new System.EventHandler(this.buttonStopRendering_Click);
			// 
			// buttonRenderAnimation
			// 
			this.buttonRenderAnimation.BackColor = System.Drawing.Color.DimGray;
			this.buttonRenderAnimation.Enabled = false;
			this.buttonRenderAnimation.FlatStyle = System.Windows.Forms.FlatStyle.Flat;
			this.buttonRenderAnimation.ForeColor = System.Drawing.Color.LightSteelBlue;
			this.buttonRenderAnimation.Location = new System.Drawing.Point(264, 80);
			this.buttonRenderAnimation.Name = "buttonRenderAnimation";
			this.buttonRenderAnimation.Size = new System.Drawing.Size(88, 40);
			this.buttonRenderAnimation.TabIndex = 5;
			this.buttonRenderAnimation.Text = "Render Animation";
			this.toolTip1.SetToolTip(this.buttonRenderAnimation, "Render the entire animation using the settings in the scene");
			this.buttonRenderAnimation.Click += new System.EventHandler(this.buttonRenderAnimation_Click);
			// 
			// pictureBox1
			// 
			this.pictureBox1.Cursor = System.Windows.Forms.Cursors.Hand;
			this.pictureBox1.Image = ((System.Drawing.Image)(resources.GetObject("pictureBox1.Image")));
			this.pictureBox1.Location = new System.Drawing.Point(0, 0);
			this.pictureBox1.Name = "pictureBox1";
			this.pictureBox1.Size = new System.Drawing.Size(350, 80);
			this.pictureBox1.TabIndex = 6;
			this.pictureBox1.TabStop = false;
			this.toolTip1.SetToolTip(this.pictureBox1, "Click me!");
			this.pictureBox1.Click += new System.EventHandler(this.pictureBox1_Click);
			// 
			// Form1
			// 
			this.AutoScaleBaseSize = new System.Drawing.Size(5, 13);
			this.BackColor = System.Drawing.Color.Black;
			this.ClientSize = new System.Drawing.Size(346, 141);
			this.Controls.Add(this.pictureBox1);
			this.Controls.Add(this.buttonRenderAnimation);
			this.Controls.Add(this.buttonStopRendering);
			this.Controls.Add(this.buttonPauseRendering);
			this.Controls.Add(this.buttonPredict);
			this.Controls.Add(this.buttonLoadScene);
			this.Controls.Add(this.buttonRender);
			this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedSingle;
			this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
			this.MaximizeBox = false;
			this.Name = "Form1";
			this.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
			this.Text = "R.I.S.E. Scene Renderer";
			this.Closing += new System.ComponentModel.CancelEventHandler(this.Form1_Closing);
			this.Load += new System.EventHandler(this.Form1_Load);
			this.ResumeLayout(false);

		}
		#endregion

		/// <summary>
		/// The main entry point for the application.
		/// </summary>
		[STAThread]
		static void Main() 
		{
			Application.Run(new Form1());
		}

		private void LoadThread()
		{
			bool bPredictEnabled = buttonPredict.Enabled;
			bool bRenderEnabled = buttonRender.Enabled;
			bool bRenderAnimationEnabled = buttonRenderAnimation.Enabled;

			buttonLoadScene.Enabled = false;

			buttonPredict.Enabled = false;
			buttonRender.Enabled = false;
			buttonRenderAnimation.Enabled = false;

			OpenFileDialog openFileDialog1 = new OpenFileDialog();

			openFileDialog1.Filter = "Scenes (*.RISEScene)|*.RISEScene|All files (*.*)|*.*" ;
			openFileDialog1.FilterIndex = 1;
			openFileDialog1.RestoreDirectory = false;

			if(openFileDialog1.ShowDialog() == DialogResult.OK)
			{
				System.GC.Collect();

				string savedDirectory = System.Environment.CurrentDirectory;
				System.Environment.CurrentDirectory = System.IO.Path.GetDirectoryName(openFileDialog1.FileName);				

				System.Windows.Forms.Cursor oldCursor = System.Windows.Forms.Cursor.Current;
				System.Windows.Forms.Cursor.Current = System.Windows.Forms.Cursors.WaitCursor;

				job.ClearAll();
				if( !job.LoadAsciiScene( openFileDialog1.FileName ) ) 
				{
					MessageBox.Show( "Failed to load scene `" + openFileDialog1.FileName + "`", "R.I.S.E.", MessageBoxButtons.OK, MessageBoxIcon.Error );
				} 
				else 
				{
					sceneFile = openFileDialog1.FileName;
					job.AddWindowRasterizerOutput( "R.I.S.E. .NET Render Output", 20, 30 );
					buttonPredict.Enabled = true;
					buttonRender.Enabled = true;
					buttonRenderAnimation.Enabled = job.AreThereAnyKeyframedObjects();
				}

				System.Environment.CurrentDirectory = savedDirectory;
				System.Windows.Forms.Cursor.Current = oldCursor;
			} 
			else 
			{
				buttonPredict.Enabled = bPredictEnabled;
				buttonRender.Enabled = bRenderEnabled;
				buttonRenderAnimation.Enabled = bRenderAnimationEnabled;
			}

			buttonLoadScene.Enabled = true;
		}

		private void buttonLoadScene_Click(object sender, System.EventArgs e)
		{
			Thread loadThread = new Thread( new ThreadStart( LoadThread ) );
			loadThread.Start();
		}

		private void buttonPredict_Click(object sender, System.EventArgs e)
		{
			if( job != null ) 
			{
				System.Windows.Forms.Cursor oldCursor = System.Windows.Forms.Cursor.Current;
				System.Windows.Forms.Cursor.Current = System.Windows.Forms.Cursors.WaitCursor;

				uint ms = job.PredictRasterizationTime( 4096 );

				System.Windows.Forms.Cursor.Current = oldCursor;

				if( ms > 0 ) 
				{
					uint sec = (ms / 1000) % 60;
					uint min = ms / 60000;
					ms = ms % 1000;

					MessageBox.Show ("Predicted time to render the scene: " + min + " minutes, " + sec + " seconds, " + ms + " ms.", "R.I.S.E", 
						MessageBoxButtons.OK, MessageBoxIcon.Asterisk);
				}
			}
		}

		private void RenderThread()
		{
			System.String savedDirectory = System.Environment.CurrentDirectory;

			if( sceneFile != null )
			{
				System.Environment.CurrentDirectory = System.IO.Path.GetDirectoryName(sceneFile);				
			}

			job.Rasterize();

			System.Environment.CurrentDirectory = savedDirectory;

			buttonLoadScene.Enabled = true;
			buttonRender.Enabled = true;
			buttonRenderAnimation.Enabled = job.AreThereAnyKeyframedObjects();
			buttonPredict.Enabled = true;

			buttonPauseRendering.Enabled = false;
			buttonStopRendering.Enabled = false;
		}

		private void RenderAnimationThread()
		{
			System.String savedDirectory = System.Environment.CurrentDirectory;

			if( sceneFile != null )
			{
				System.Environment.CurrentDirectory = System.IO.Path.GetDirectoryName(sceneFile);				
			}

			job.RasterizeAnimationUsingOptions();

			System.Environment.CurrentDirectory = savedDirectory;

			buttonLoadScene.Enabled = true;
			buttonRender.Enabled = true;
			buttonRenderAnimation.Enabled = job.AreThereAnyKeyframedObjects();
			buttonPredict.Enabled = true;

			buttonPauseRendering.Enabled = false;
			buttonStopRendering.Enabled = false;
		}

		private void buttonRender_Click(object sender, System.EventArgs e)
		{
			buttonLoadScene.Enabled = false;
			buttonRender.Enabled = false;
			buttonRenderAnimation.Enabled = false;
			buttonPredict.Enabled = false;

			renderThread = new Thread( new ThreadStart( RenderThread ) );
			renderThread.IsBackground = true;
			renderThread.Start();
			buttonPauseRendering.Enabled = true;
			buttonStopRendering.Enabled = true;
		}

		private void buttonPauseRendering_Click(object sender, System.EventArgs e)
		{
			if( renderThread != null )
			{
				if( buttonPauseRendering.Text == "Pause Rendering" )
				{
					renderThread.Suspend();
					buttonPauseRendering.Text = "Resume Rendering";
					buttonStopRendering.Enabled = false;
				} 
				else 
				{
					renderThread.Resume();
					buttonPauseRendering.Text = "Pause Rendering";
					buttonStopRendering.Enabled = true;
				}
			}
		}

		private void Form1_Closing(object sender, System.ComponentModel.CancelEventArgs e)
		{
			job.DestroyAllPrinters();
			if( renderThread != null )
			{
				if( renderThread.ThreadState == ThreadState.Suspended || 
					renderThread.ThreadState == (ThreadState)68 )
				{
					renderThread.Resume();
				}
				renderThread.Abort();
				renderThread = null;
			}
		}

		private void buttonStopRendering_Click(object sender, System.EventArgs e)
		{
			if( renderThread != null )
			{
				System.Windows.Forms.Cursor oldCursor = System.Windows.Forms.Cursor.Current;
				System.Windows.Forms.Cursor.Current = System.Windows.Forms.Cursors.WaitCursor;

				renderThread.Abort();
				renderThread.Join();
				job = null;
				job = new RISE.Job();

				buttonLoadScene.Enabled = true;
				buttonRender.Enabled = false;
				buttonRenderAnimation.Enabled = false;
				buttonPredict.Enabled = false;

				buttonPauseRendering.Enabled = false;
				buttonStopRendering.Enabled = false;

				System.Windows.Forms.Cursor.Current = oldCursor;
			}
		}

		private void Form1_Load(object sender, System.EventArgs e)
		{
			if( job == null )
			{
				job = new RISE.Job();
			}
			job.AddWin32ConsoleLogPrinter( "R.I.S.E. Log", 800, 400, 20, 30, true );
		}

		private void buttonRenderAnimation_Click(object sender, System.EventArgs e)
		{
			buttonLoadScene.Enabled = false;
			buttonRender.Enabled = false;
			buttonRenderAnimation.Enabled = false;
			buttonPredict.Enabled = false;

			renderThread = new Thread( new ThreadStart( RenderAnimationThread ) );
			renderThread.IsBackground = true;
			renderThread.Start();
			buttonPauseRendering.Enabled = true;
			buttonStopRendering.Enabled = true;
		}

		private void pictureBox1_Click(object sender, System.EventArgs e)
		{
			int [] major = new int[1];
			int [] minor = new int[1];
			int [] revision = new int[1];
			int [] build = new int[1];
			bool [] debug = new bool[1];
			job.GetVersion( major, minor, revision, build, debug );	
			MessageBox.Show(	job.GetCopyrightInformation()+"\n"+
								"version "+major[0]+"."+minor[0]+"."+revision[0]+" build "+build[0]+"\n"+
								"Built on: "+job.GetBuildDate()+" at "+job.GetBuildTime()+" PST\n\n"+
								"For questions or comments, email: rise@aravind.ca\n"+
								"http://rise.sf.net\n\n"+
								"Please note:  This software comes makes no warranties or guarantees\n"+
								"implied or otherwise.  This software is provided AS-IS and you may use it\n"+
								"at your own risk.  Neither Aravind Krishnaswamy nor any developer of R.I.S.E.\n"+
								"may be held liable for any damages or loss incurred by the use or lack of use of this software.", "R.I.S.E." );
		}
	}
}
