#include "openmc/initialize.h"

#include <cstddef>
#include <cstdlib> // for getenv
#include <cstring>
#include <string>

#ifdef _OPENMP
#include <omp.h>
#endif
#include <fmt/core.h>

#include "openmc/capi.h"
#include "openmc/constants.h"
#include "openmc/cross_sections.h"
#include "openmc/error.h"
#include "openmc/file_utils.h"
#include "openmc/geometry_aux.h"
#include "openmc/hdf5_interface.h"
#include "openmc/material.h"
#include "openmc/memory.h"
#include "openmc/message_passing.h"
#include "openmc/mgxs_interface.h"
#include "openmc/nuclide.h"
#include "openmc/output.h"
#include "openmc/plot.h"
#include "openmc/random_lcg.h"
#include "openmc/settings.h"
#include "openmc/simulation.h"
#include "openmc/string_utils.h"
#include "openmc/summary.h"
#include "openmc/tallies/tally.h"
#include "openmc/thermal.h"
#include "openmc/timer.h"
#include "openmc/vector.h"

#ifdef LIBMESH
#include "libmesh/libmesh.h"
#endif

int openmc_init(int argc, char* argv[], const void* intracomm)
{
  using namespace openmc;

#ifdef OPENMC_MPI
  // Check if intracomm was passed
  MPI_Comm comm;
  if (intracomm) {
    comm = *static_cast<const MPI_Comm*>(intracomm);
  } else {
    comm = MPI_COMM_WORLD;
  }

  // Initialize MPI for C++
  initialize_mpi(comm);
#endif

  // Parse command-line arguments
  int err = parse_command_line(argc, argv);
  if (err)
    return err;

#ifdef LIBMESH

#ifdef _OPENMP
  int n_threads = omp_get_max_threads();
#else
  int n_threads = 1;
#endif

  // initialize libMesh if it hasn't been initialized already
  // (if initialized externally, the libmesh_init object needs to be provided
  // also)
  if (!settings::libmesh_init && !libMesh::initialized()) {
#ifdef OPENMC_MPI
    // pass command line args, empty MPI communicator, and number of threads.
    // Because libMesh was not initialized, we assume that OpenMC is the primary
    // application and that its main MPI comm should be used.
    settings::libmesh_init =
      make_unique<libMesh::LibMeshInit>(argc, argv, comm, n_threads);
#else
    // pass command line args, empty MPI communicator, and number of threads
    settings::libmesh_init =
      make_unique<libMesh::LibMeshInit>(argc, argv, 0, n_threads);
#endif

    settings::libmesh_comm = &(settings::libmesh_init->comm());
  }

#endif

  // Start total and initialization timer
  simulation::time_total.start();
  simulation::time_initialize.start();

#ifdef _OPENMP
  // If OMP_SCHEDULE is not set, default to a static schedule
  char* envvar = std::getenv("OMP_SCHEDULE");
  if (!envvar) {
    omp_set_schedule(omp_sched_static, 0);
  }
#endif

  // Initialize random number generator -- if the user specifies a seed, it
  // will be re-initialized later
  openmc::openmc_set_seed(DEFAULT_SEED);

  // Read XML input files
  if (!read_model_xml()) read_separate_xml_files();

  // Write some initial output under the header if needed
  initial_output();

  // Check for particle restart run
  if (settings::particle_restart_run)
    settings::run_mode = RunMode::PARTICLE;

  // Stop initialization timer
  simulation::time_initialize.stop();
  simulation::time_total.stop();

  return 0;
}

namespace openmc {

#ifdef OPENMC_MPI
void initialize_mpi(MPI_Comm intracomm)
{
  mpi::intracomm = intracomm;

  // Initialize MPI
  int flag;
  MPI_Initialized(&flag);
  if (!flag)
    MPI_Init(nullptr, nullptr);

  // Determine number of processes and rank for each
  MPI_Comm_size(intracomm, &mpi::n_procs);
  MPI_Comm_rank(intracomm, &mpi::rank);
  mpi::master = (mpi::rank == 0);

  // Create bank datatype
  SourceSite b;
  MPI_Aint disp[10];
  MPI_Get_address(&b.r, &disp[0]);
  MPI_Get_address(&b.u, &disp[1]);
  MPI_Get_address(&b.E, &disp[2]);
  MPI_Get_address(&b.time, &disp[3]);
  MPI_Get_address(&b.wgt, &disp[4]);
  MPI_Get_address(&b.delayed_group, &disp[5]);
  MPI_Get_address(&b.surf_id, &disp[6]);
  MPI_Get_address(&b.particle, &disp[7]);
  MPI_Get_address(&b.parent_id, &disp[8]);
  MPI_Get_address(&b.progeny_id, &disp[9]);
  for (int i = 9; i >= 0; --i) {
    disp[i] -= disp[0];
  }

  int blocks[] {3, 3, 1, 1, 1, 1, 1, 1, 1, 1};
  MPI_Datatype types[] {MPI_DOUBLE, MPI_DOUBLE, MPI_DOUBLE, MPI_DOUBLE,
    MPI_DOUBLE, MPI_INT, MPI_INT, MPI_INT, MPI_LONG, MPI_LONG};
  MPI_Type_create_struct(10, blocks, disp, types, &mpi::source_site);
  MPI_Type_commit(&mpi::source_site);
}
#endif // OPENMC_MPI

int parse_command_line(int argc, char* argv[])
{
  int last_flag = 0;
  for (int i = 1; i < argc; ++i) {
    std::string arg {argv[i]};
    if (arg[0] == '-') {
      if (arg == "-p" || arg == "--plot") {
        settings::run_mode = RunMode::PLOTTING;
        settings::check_overlaps = true;

      } else if (arg == "-n" || arg == "--particles") {
        i += 1;
        settings::n_particles = std::stoll(argv[i]);

      } else if (arg == "-e" || arg == "--event") {
        settings::event_based = true;
      } else if (arg == "-i" || arg == "--input") {
        i += 1;
        args_xml_filename = std::string(argv[i]);
      } else if (arg == "-r" || arg == "--restart") {
        i += 1;
        // Check what type of file this is
        hid_t file_id = file_open(argv[i], 'r', true);
        std::string filetype;
        read_attribute(file_id, "filetype", filetype);
        file_close(file_id);

        // Set path and flag for type of run
        if (filetype == "statepoint") {
          settings::path_statepoint = argv[i];
          settings::restart_run = true;
        } else if (filetype == "particle restart") {
          settings::path_particle_restart = argv[i];
          settings::particle_restart_run = true;
        } else {
          auto msg =
            fmt::format("Unrecognized file after restart flag: {}.", filetype);
          strcpy(openmc_err_msg, msg.c_str());
          return OPENMC_E_INVALID_ARGUMENT;
        }

        // If its a restart run check for additional source file
        if (settings::restart_run && i + 1 < argc) {
          // Check if it has extension we can read
          if (ends_with(argv[i + 1], ".h5")) {

            // Check file type is a source file
            file_id = file_open(argv[i + 1], 'r', true);
            read_attribute(file_id, "filetype", filetype);
            file_close(file_id);
            if (filetype != "source") {
              std::string msg {
                "Second file after restart flag must be a source file"};
              strcpy(openmc_err_msg, msg.c_str());
              return OPENMC_E_INVALID_ARGUMENT;
            }

            // It is a source file
            settings::path_sourcepoint = argv[i + 1];
            i += 1;

          } else {
            // Source is in statepoint file
            settings::path_sourcepoint = settings::path_statepoint;
          }

        } else {
          // Source is assumed to be in statepoint file
          settings::path_sourcepoint = settings::path_statepoint;
        }

      } else if (arg == "-g" || arg == "--geometry-debug") {
        settings::check_overlaps = true;
      } else if (arg == "-c" || arg == "--volume") {
        settings::run_mode = RunMode::VOLUME;
      } else if (arg == "-s" || arg == "--threads") {
        // Read number of threads
        i += 1;

#ifdef _OPENMP
        // Read and set number of OpenMP threads
        int n_threads = std::stoi(argv[i]);
        if (n_threads < 1) {
          std::string msg {"Number of threads must be positive."};
          strcpy(openmc_err_msg, msg.c_str());
          return OPENMC_E_INVALID_ARGUMENT;
        }
        omp_set_num_threads(n_threads);
#else
        if (mpi::master) {
          warning("Ignoring number of threads specified on command line.");
        }
#endif

      } else if (arg == "-?" || arg == "-h" || arg == "--help") {
        print_usage();
        return OPENMC_E_UNASSIGNED;

      } else if (arg == "-v" || arg == "--version") {
        print_version();
        print_build_info();
        return OPENMC_E_UNASSIGNED;

      } else if (arg == "-t" || arg == "--track") {
        settings::write_all_tracks = true;

      } else {
        fmt::print(stderr, "Unknown option: {}\n", argv[i]);
        print_usage();
        return OPENMC_E_UNASSIGNED;
      }

      last_flag = i;
    }
  }

  // Determine directory where XML input files are
  if (argc > 1 && last_flag < argc - 1) {
    settings::path_input = std::string(argv[last_flag + 1]);

    // Add slash at end of directory if it isn't there
    if (!ends_with(settings::path_input, "/")) {
      settings::path_input += "/";
    }
  }

  return 0;
}

bool read_model_xml() {
  // get verbosity from settings node

  std::string xml_filename = "model.xml";
  if (!args_xml_filename.empty()) xml_filename = args_xml_filename;
  std::string model_filename = settings::path_input + xml_filename;

  // check that the model file exists. If it does not and a custom filename was
  // supplied by the user report an error
  if (!file_exists(model_filename)) {
    if (!args_xml_filename.empty()) {
      fatal_error(fmt::format("The input file '{}' specified on the command line does not exist", args_xml_filename));
    }
    return false;
  }

  // attempt to open the document
  pugi::xml_document doc;
  auto result = doc.load_file(model_filename.c_str());
  if (!result) {
    fatal_error("Error processing model.xml file.");
  }
  pugi::xml_node root = doc.document_element();

  // Read settings
  if (!check_for_node(root, "settings")) {
      fatal_error("No <settings> node present in the model.xml file.");
  }
  auto settings_root = root.child("settings");

  // Verbosity
  if (check_for_node(settings_root, "verbosity")) {
    settings::verbosity = std::stoi(get_node_value(settings_root, "verbosity"));
  }

  // To this point, we haven't displayed any output since we didn't know what
  // the verbosity is. Now that we checked for it, show the title if necessary
  if (mpi::master) {
    if (settings::verbosity >= 2)
      title();
  }

  if (!args_xml_filename.empty())
  write_message(fmt::format("Reaging user-specified input '{}'...", args_xml_filename), 5);
  else
  write_message("Reading model XML file...", 5);

  read_settings_xml(settings_root);

  // If other XML files are present, display warning
  // that they will be ignored
  auto other_inputs = {"materials.xml", "geometry.xml", "settings.xml", "tallies.xml", "plots.xml"};
  for (const auto& input : other_inputs) {
    if (file_exists(settings::path_input + input)) {
      warning((fmt::format("Other XML file input(s) are present. These file will be ignored in favor of the {} file.", xml_filename));
      break;
    }
  }

  // Read materials and cross sections
  if (!check_for_node(root, "materials")) {
    fatal_error(
      fmt::format("No <materials> node present in the {} file.", xml_filename));
  }

  // find materials node this object cannot be used after being passed to the
  // cross section reading function below
  auto materials_node = root.child("materials");
  read_cross_sections_xml(materials_node);

  read_materials_xml(root.child("materials"));

  // Read geometry
  if (!check_for_node(root, "geometry")) {
    fatal_error(fmt::format(
      "No <geometry> node present in the model.xml_file.", xml_filename));
  }
  read_geometry_xml(root.child("geometry"));

  // Final geometry setup and assign temperatures
  finalize_geometry();

  // Finalize cross sections having assigned temperatures
  finalize_cross_sections();

  if (check_for_node(root, "tallies"))
    read_tallies_xml(root.child("tallies"));

 // Initialize distribcell_filters
  prepare_distribcell();

  if (check_for_node(root, "plots"))
    read_plots_xml(root.child("plots"));

  return true;
}

void read_separate_xml_files()
{
  read_settings_xml();
  read_cross_sections_xml();
  read_materials_xml();
  read_geometry_xml();

  // Final geometry setup and assign temperatures
  finalize_geometry();

  // Finalize cross sections having assigned temperatures
  finalize_cross_sections();

  read_tallies_xml();

  // Initialize distribcell_filters
  prepare_distribcell();

  // Read the plots.xml regardless of plot mode in case plots are requested
  // via the API
  read_plots_xml();
}

void initial_output() {
  // write initial output
  if (settings::run_mode == RunMode::PLOTTING) {
    // Read plots.xml if it exists
    if (mpi::master && settings::verbosity >= 5)
      print_plot();

  } else {
    // Write summary information
    if (mpi::master && settings::output_summary)
      write_summary();

    // Warn if overlap checking is on
    if (mpi::master && settings::check_overlaps) {
      warning("Cell overlap checking is ON.");
    }
  }
}

} // namespace openmc
