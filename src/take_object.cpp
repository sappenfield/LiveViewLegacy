#include "take_object.hpp"
#include "fft.hpp"

take_object::take_object(int channel_num, int number_of_buffers, int frf)
{
#ifdef EDT
    this->channel = channel_num;
    this->numbufs = number_of_buffers;
    this->filter_refresh_rate =frf;
#endif

    frame_ring_buffer = new frame_c[CPU_FRAME_BUFFER_SIZE];

    //For the filters
    dsfMaskCollected = false;
    this->std_dev_filter_N = 400;
    whichFFT = PLANE_MEAN;

    //For the frame saving
    this->do_raw_save = false;
    save_framenum = 0;
    saving_list.clear();
}
take_object::~take_object()
{
    if(pdv_thread_run != 0) {
        pdv_thread_run = 0;

#ifdef OPALKELLY
        usleep(1000);
#endif

#ifdef EDT
        int dummy;
        pdv_wait_last_image(pdv_p,&dummy); //Collect the last frame to avoid core dump
        pdv_close(pdv_p);
#endif

#ifdef VERBOSE
        printf("about to delete filters!\n");
#endif

        delete dsf;
        delete sdvf;
    }

    delete[] frame_ring_buffer;

#ifdef RESET_GPUS
    printf("reseting GPUs!\n");
    int count;
    cudaGetDeviceCount(&count);
    for(int i = 0; i < count; i++) {
        printf("resetting GPU#%i",i);
        cudaSetDevice(i);
        cudaDeviceReset(); //Dump all the bad stuff from each of our GPUs.
    }
#endif
}

//public functions
void take_object::start()
{
    pdv_thread_run = 1;

    std::cout << "This version of cuda_take was compiled on " << __DATE__ << " at " << __TIME__ << " using gcc " << __GNUC__ << std::endl;
    std::cout << "The compilation was perfromed by " << UNAME << " @ " << HOST << std::endl;

#ifdef EDT
    this->pdv_p = NULL;
    this->pdv_p = pdv_open_channel(EDT_INTERFACE,0,this->channel);
    if(pdv_p == NULL) {
        std::cerr << "Could not open device channel. Is one connected?" << std::endl;
        return;
    }
    size = pdv_get_dmasize(pdv_p); // this size is only used to determine the camera type
    // actual grabbing of the dimensions
    frWidth = pdv_get_width(pdv_p);
    dataHeight = pdv_get_height(pdv_p);
#endif
#ifdef OPALKELLY
    // Step 1: Check that the Opal Kelly DLL is loaded
    if(!okFrontPanelDLL_LoadLib(NULL)) {
        std::cerr << "Front Panel DLL could not be loaded." << std::endl;
        return;
    }

    // Step 2: Initialize the device, and check that the initialization was valid
    xem = initializeFPGA();
    if(!xem)
        return;

    // Step 3: Initialize some values that we will need to hardcode for this device
    frWidth = NCOL;
    frHeight = NROW;
    dataHeight = frHeight;
    size = frHeight * frWidth * sizeof(uint16_t);
    framelen = 2 * frWidth * frHeight / NCHUNX;
    blocklen = BLOCK_LEN;

    // Step 4: Begin writing values to the pipe
    ok_init_pipe();
#endif

    switch(size) {
    case 481*640*sizeof(uint16_t): cam_type = CL_6604A; break;
    case 285*640*sizeof(uint16_t): cam_type = CL_6604A; break;
    case 480*640*sizeof(uint16_t): cam_type = FPGA; pixRemap = true; break;
    default: cam_type = CL_6604B; pixRemap = true; break;
    }
	setup_filter(cam_type);

#ifdef EDT
    frHeight = cam_type == CL_6604A ? dataHeight - 1 : dataHeight;
#endif
#ifdef VERBOSE
    std::cout << "Camera Type: " << cam_type << ". Frame Width: " << frWidth << \
                 " Data Height: " << dataHeight << " Frame Height: " << frHeight << std::endl;
    std::cout << "About to start threads..." << std::endl;
#endif

    // Initialize the filters
    dsf = new dark_subtraction_filter(frWidth,frHeight);
    sdvf = new std_dev_filter(frWidth,frHeight);

    // Initial dimensions for calculating the mean that can be updated later
    meanStartRow = 0;
    meanStartCol = 0;
    meanHeight = frHeight;
    meanWidth = frWidth;

#ifdef EDT
    pdv_multibuf(pdv_p,this->numbufs);
    numbufs = 16;
    pdv_start_images(pdv_p,numbufs); //Before looping, emit requests to fill the pdv ring buffer
#endif

    pdv_thread = boost::thread(&take_object::pdv_loop, this);
    //saving_thread = boost::thread(&take_object::savingLoop, this); //For some reason this doesn't work atm	
	while(!pdv_thread_start_complete) usleep(1);
}
void take_object::setInversion(bool checked, unsigned int factor)
{
    inverted = checked;
    invFactor = factor;
}
void take_object::paraPixRemap(bool checked )
{
    pixRemap = checked;
}
void take_object::startCapturingDSFMask()
{
    dsfMaskCollected = false;
    dsf->start_mask_collection();
}
void take_object::finishCapturingDSFMask()
{
    dsf->mask_mutex.lock();
    dsf->finish_mask_collection();
    dsf->mask_mutex.unlock();
    dsfMaskCollected = true;
}
void take_object::loadDSFMask(std::string file_name)
{
    float *mask_in = new float[frWidth*frHeight];
    FILE *pFile;
    unsigned long size = 0;
    pFile  = fopen(file_name.c_str(), "rb");
    if(pFile == NULL) std::cerr << "error opening raw file" << std::endl;
    else
    {
        fseek (pFile, 0, SEEK_END); // non-portable
        size = ftell(pFile);
        if(size != (frWidth*frHeight*sizeof(float)))
        {
            std::cerr << "Error: mask file does not match image size" << std::endl;
            fclose (pFile);
            return;
        }
        rewind(pFile);   // go back to beginning
        fread(mask_in,sizeof(float),frWidth * frHeight,pFile);
        fclose (pFile);
#ifdef VERBOSE
        std::cout << file_name << " read in "<< size << " bytes successfully " <<  std::endl;
#endif
    }
    dsf->load_mask(mask_in);
}
void take_object::setStdDev_N(int s)
{
    this->std_dev_filter_N = s;
}
void take_object::updateVertRange(int br, int er)
{
    meanStartRow = br;
    meanHeight = er;
#ifdef VERBOSE
    std::cout << "meanStartRow: " << meanStartRow << " meanHeight: " << meanHeight << std::endl;
#endif
}
void take_object::updateHorizRange(int bc, int ec)
{
    meanStartCol = bc;
    meanWidth = ec;
#ifdef VERBOSE
    std::cout << "meanStartCol: " << meanStartCol << " meanWidth: " << meanWidth << std::endl;
#endif
}
void take_object::changeFFTtype(FFT_t t)
{
    whichFFT = t;
}
void take_object::startSavingRaws(std::string raw_file_name, unsigned int frames_to_save)
{
    save_framenum.store(0, std::memory_order_seq_cst);
#ifdef VERBOSE
    printf("ssr called\n");
#endif
    while(!saving_list.empty())
    {
#ifdef VERBOSE
        printf("Waiting for empty saving list...\n");
#endif
    }
    save_framenum.store(frames_to_save,std::memory_order_seq_cst);
#ifdef VERBOSE
    printf("Begin frame save! @ %s\n", raw_file_name.c_str());
#endif

    saving_thread = boost::thread(&take_object::savingLoop,this,raw_file_name);
}
void take_object::stopSavingRaws()
{
    save_framenum.store(0,std::memory_order_relaxed);
#ifdef VERBOSE
    printf("Stop Saving Raws!");
#endif
}
/*void take_object::panicSave( std::string raw_file_name )
{
    while(!saving_list.empty());
    boost::thread(&take_object::saveFramesInBuffer,this);
    boost::thread(&take_object::savingLoop,this,raw_file_name);
}*/
unsigned int take_object::getDataHeight()
{
    return dataHeight;
}
unsigned int take_object::getFrameHeight()
{
    return frHeight;
}
unsigned int take_object::getFrameWidth()
{
    return frWidth;
}
bool take_object::std_dev_ready()
{
    return sdvf->outputReady();
}
std::vector<float> * take_object::getHistogramBins()
{
    return sdvf->getHistogramBins();
}
FFT_t take_object::getFFTtype()
{
    return whichFFT;
}

// private functions
void take_object::pdv_loop() //Producer Thread (pdv_thread)
{
    count = 0;

    uint16_t framecount = 1;
    uint16_t last_framecount = 0;
#ifdef EDT
	unsigned char* wait_ptr;
#endif
#ifdef OPALKELLY
    unsigned char wait_ptr[framelen];
    long prev_result = 0;
#endif

    while(pdv_thread_run == 1)
    {
        curFrame = &frame_ring_buffer[count % CPU_FRAME_BUFFER_SIZE];
        curFrame->reset();
#ifdef EDT
        pdv_start_image(pdv_p); //Start another
        wait_ptr = pdv_wait_image(pdv_p);
		pdv_thread_start_complete=true;
#endif
#ifdef OPALKELLY
        prev_result = ok_read_frame(wait_ptr, prev_result);
#endif
        /* In this section of the code, after we have copied the memory from the camera link
         * buffer into the raw_data_ptr, we will check various parameters to see if we need to
         * modify the data based on our hardware.
         *
         * First, the data is stored differently depending on the type of camera, 6604A or B.
         *
         * Second, we may have to apply a filter to pixels which remaps the image based on the
         * way information is sent by some detectors.
         *
         * Third, we may need to invert the data range if a cable is inverting the magnitudes
         * that arrive from the ADC. This feature is also modified from the preference window.
         */
        memcpy(curFrame->raw_data_ptr,wait_ptr,frWidth*dataHeight*sizeof(uint16_t));
        if(pixRemap)
            apply_chroma_translate_filter(curFrame->raw_data_ptr);
        if(cam_type == CL_6604A)
            curFrame->image_data_ptr = curFrame->raw_data_ptr + frWidth;
        else
            curFrame->image_data_ptr = curFrame->raw_data_ptr;
        if(inverted)
        { // record the data from high to low. Store the pixel buffer in INVERTED order from the camera link
            for(uint i = 0; i < frHeight*frWidth; i++ )
                curFrame->image_data_ptr[i] = invFactor - curFrame->image_data_ptr[i];
        }

        // Calculating the filters for this frame
        sdvf->update_GPU_buffer(curFrame,std_dev_filter_N);
        dsf->update(curFrame->raw_data_ptr,curFrame->dark_subtracted_data);
        mean_filter * mf = new mean_filter(curFrame,count,meanStartCol,meanWidth,meanStartRow,meanHeight,frWidth,useDSF,whichFFT);
        //This will deallocate itself when it is done.
        mf->start_mean();

        if(save_framenum > 0)
        {
            uint16_t * raw_copy = new uint16_t[frWidth*dataHeight];
            memcpy(raw_copy,curFrame->raw_data_ptr,frWidth*dataHeight*sizeof(uint16_t));
            saving_list.push_front(raw_copy);
            save_framenum--;
        }

#ifdef EDT

#endif

        framecount = *(curFrame->raw_data_ptr + 160); // The framecount is stored 160 bytes offset from the beginning of the data
        if(CHECK_FOR_MISSED_FRAMES_6604A && cam_type == CL_6604A)
        {
            if( (framecount - 1 != last_framecount) && (last_framecount != UINT16_MAX) )
            {
                std::cerr << "WARNING: MISSED FRAME " << framecount << std::endl;
            }
        }
        last_framecount = framecount;
        count++;
    }
}
void take_object::savingLoop(std::string fname) //Frame Save Thread (saving_thread)
{
	std::size_t pos = fname.find(".");      // position of "." in str
  	fname = fname.substr(0, pos) + ".raw";
  	
  	std::string hdr_fname 
  	hdr_fname = fname.substr(0,fname.size()-3) + "hdr";   
    
    FILE * file_target = fopen(fname.c_str(), "wb");
    FILE * file_hdr_target = fopen(hdr_fname.c_str(), "w");
	sv_count = 0;
	
    while(save_framenum != 0 || !saving_list.empty())
    {
        if(!saving_list.empty())
        {
        	if(NUM_AVGS_SAVE = 1)
        	{
	            uint16_t * data = saving_list.back();
	            saving_list.pop_back();
	            fwrite(data,sizeof(uint16_t),frWidth*dataHeight,file_target); //It is ok if this blocks
	            delete[] data;
	            sv_count++;
        	}
        	else-if(saving_list.size() >= NUM_AVGS_SAVE)
        	{
        		float * data = new float[frWidth*dataHeight];
        		for(unsigned int i2 = 0, i2<NUM_AVGS_SAVE; i2++)
        		{
					uint16_t * data2 = saving_list.back();
					saving_list.pop_back();
	        		if(i2 = 1)
	        		{
		        		for(unsigned int i = 0; i<frWidth*dataHeight; i++)
						{
				        	data[i] = (float)data2[i];
						}
	        		}
	        		else-if(i2 = NUM_AVGS_SAVE)
	        		{
		        		for(unsigned int i = 0; i<frWidth*dataHeight; i++)
						{
				        	data[i] = (data[i] + (float)data2[i])/NUM_AVGS_SAVE;
						}
	        		}
	        		else
	        		{
		        		for(unsigned int i = 0; i<frWidth*dataHeight; i++)
						{
				        	data[i] += (float)data2[i];
						}
	        		}
	        		delete[] data;
        		}
	            fwrite(data,sizeof(float),frWidth*dataHeight,file_target); //It is ok if this blocks
	            delete[] data;
	            sv_count++;
	            if(save_framenum = 0 && saving_list.size() < NUM_AVGS_SAVE)
	            {
	            	saving_list.erase(saving_list.begin(),saving_list.end()) 
	            }
        	}
	        else
	        {
	            //We're waiting for data to get added to the list...
	            usleep(250);
	        }
        }
        else
        {
            //We're waiting for data to get added to the list...
            usleep(250);
        }
    }

    //We're done!
    fclose(file_target);
    printf("Frame save complete!\n");
    std:string hdr_text = "ENVIdescription = {LIVEVIEW raw export file}";
	hdr_text= hdr_text + "\n" + "samples = 1280";
	lines   = $fnum
	bands   = 480
	header offset = 0
	file type = ENVI Standard
	data type = 12
	interleave = bil
	sensor type = Unknown
	byte order = 0
	wavelength units = Unknown
	"
    fwrite()
}
/*void take_object::saveFramesInBuffer()
{
    register int ndx = count % CPU_FRAME_BUFFER_SIZE;
    // We need to make a copy of the frame ring buffer so that we can
    // take the raws out of the frames. So we should copy frames starting from the current framecount
    // to the end of the array then from the beginning of the array to the current framecount.

    frame_c* buf_copy = new frame_c[CPU_FRAME_BUFFER_SIZE];
    memcpy(buf_copy + ndx*sizeof(frame_c),frame_ring_buffer + ndx*sizeof(frame_c),(CPU_FRAME_BUFFER_SIZE - ndx)*sizeof(frame_c));
    memcpy(buf_copy,frame_ring_buffer,(ndx -1)*sizeof(frame_c));
    // we need to do one iteration of the saving loop first so that it doesn't immediately end the while loop...
    uint16_t raw_copy[frWidth*frHeight];
    memcpy(raw_copy,(buf_copy[ndx++]).raw_data_ptr,frWidth*dataHeight*sizeof(uint16_t));
    saving_list.push_front(raw_copy);
    while( ndx != (int)(count % CPU_FRAME_BUFFER_SIZE) )
    {
        uint16_t raw_copy[frWidth*frHeight];
        memcpy(raw_copy,(buf_copy[ndx++]).raw_data_ptr,frWidth*dataHeight*sizeof(uint16_t));
        saving_list.push_front(raw_copy);
        if( ndx == CPU_FRAME_BUFFER_SIZE )
            ndx = 0;
    }
    delete[] buf_copy;
}*/
#ifdef OPALKELLY
okCFrontPanel* take_object::initializeFPGA()
{
    // Open the first XEM - try all board types.
    okCFrontPanel *dev = new okCFrontPanel;
    if (okCFrontPanel::NoError != dev->OpenBySerial()) {
        std::cerr << "Could not open device through serial port. Is one connected?" << std::endl;
        delete dev;
        return NULL;
    }
    std::cout << "Found a device: " << dev->GetBoardModelString(dev->GetBoardModel()).c_str() << std::endl;

    dev->LoadDefaultPLLConfiguration();

    // Get some general information about the XEM.
    std::string str;
    std::cout << "Device firmware version: " << dev->GetDeviceMajorVersion() << "." << dev->GetDeviceMinorVersion() << std::endl;
    str = dev->GetSerialNumber();
    std::cout << "Device serial number: " << str.c_str() << std::endl;
    str = dev->GetDeviceID();
    std::cout << "Device device ID: " << str.c_str() << std::endl;

    // Download the configuration file.
    if(okCFrontPanel::NoError != dev->ConfigureFPGA(FPGA_CONFIG_FILE)) {
        std::cerr << "FPGA configuration failed." << std::endl;
        delete dev;
        return(NULL);
    }

    // Check for FrontPanel support in the FPGA configuration.
    if (dev->IsFrontPanelEnabled())
        std::cout << "FrontPanel support is enabled." << std::endl;
    else
        std::cerr << "FrontPanel support is not enabled." << std::endl;

    return dev;
}
void take_object::ok_init_pipe()
{
    xem->SetWireInValue(0x00, clock_div|clock_delay);
    xem->SetWireInValue(0x01, FIFO_THRESH);
    xem->UpdateWireIns();
    do {
        xem->UpdateWireOuts();
        usleep(250);
    } while(xem->GetWireOutValue(FIFO_STATUS_REG) & FIFO_WRITE_MASK);
}
long take_object::ok_read_frame(unsigned char *wait_ptr, long prev_result)
{
    xem->ActivateTriggerIn(FIFO_RESET_TRIG, 0);
    xem->ActivateTriggerIn(ACQ_TRIG, 0);
    xem->UpdateWireOuts();
    if((xem->GetWireOutValue(FIFO_STATUS_REG) & FIFO_FULL_MASK) || (prev_result != framelen)) {
        std::cerr << "WARNING: MISSED FRAME" << std::endl;

        // Reset the buffer
        xem->ActivateTriggerIn(FIFO_RESET_TRIG, 0);
        usleep(10000);
        xem->ActivateTriggerIn(ACQ_TRIG, 0);
    }
    //Check for FIFO_WRITE
    while(!(xem->GetWireOutValue(FIFO_STATUS_REG) & FIFO_WRITE_MASK)) {
        xem->UpdateWireOuts();
        usleep(1);
    }
    long result = xem->ReadFromBlockPipeOut(0xA0, blocklen, framelen, wait_ptr);
    // std::cout << "Result of ReadFromBlockPipeOut = " << result << std::endl;
    return result;
}
#endif
