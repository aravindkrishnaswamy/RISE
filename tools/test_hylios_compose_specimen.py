"""Analytic dimensional and mask oracles for the labeled clean-room composition."""
from __future__ import annotations

import copy
import json
import math
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Barrier
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hylios_import_data import CURVE_SPECS
from hylios_compose_specimen import (COMPOSITION_REVISION, LAYER_ORDER, PRESETS_PATH,
                                     StrictImportedCurves, _product, _spheroid_surface_volume,
                                     compose_specimen, write_specimen)

# Constant spectra deliberately distinct from physiological data. These isolate
# unit/concentration factors rather than compare implementation to itself.
VALUES = {name:float(i+1) for i,name in enumerate(CURVE_SPECS)}
VALUES["palmer_williams_real_ior"] = 1.3


def fixtures():
    curves = {}
    for key,spec in CURVE_SPECS.items():
        curves[key] = {"schema_version":1,"curve_id":key,
                       "source_quantity":spec["quantity"],"source_unit":spec["unit"],
                       "reference_basis":spec["reference_basis"],
                       "wavelength_nm":[250.,1300.,2500.],"values":[VALUES[key]]*3,
                       "provenance":{"source_files":{f"synthetic/{key}":"a"*64}}}
    # Importer's optional converted fields must never be mistaken for raw values.
    for key in ("dna_abs","keratin_abs","uro_acid_abs"):
        curves[key]["reference_absorption_coefficient"] = {"unit":"m^-1","values":[9e99]*3}
    for key in ("water_absorption","lipid_absorption"):
        curves[key]["si_absorption_coefficient"] = {"unit":"m^-1","values":[9e99]*3}
    return curves


class HyLIoSCompositionTest(unittest.TestCase):
    def setUp(self):
        self.curves = fixtures()
        self.tables = json.loads(PRESETS_PATH.read_text())

    def compose(self, preset="S1", waves=None, curves=None):
        return compose_specimen(StrictImportedCurves(self.curves if curves is None else curves),
                                preset, waves or [500.,1300.],presets=self.tables)

    def test_exact_sc_dimensional_sum_and_independent_weights(self):
        result = self.compose()
        layer = result["layers"][0]
        L=math.log(10)
        expected = {"water":.35*100*VALUES["water_absorption"],
                    "lipid":.20*100*VALUES["lipid_absorption"],
                    "keratin":.65*L*VALUES["keratin_abs"]/4e-6,
                    "urocanic_acid":L*VALUES["uro_acid_abs"]/.01*(.01/15e-6),
                    "beta_carotene":100*L*VALUES["beta_carotene"]*.00021/537}
        for key,value in expected.items():
            self.assertAlmostEqual(layer["constituent_absorption_m_inv"][key][0],value,
                                   msg=f"SC {key} must receive source-basis conversion exactly once")
        self.assertAlmostEqual(layer["host_absorption_m_inv"][0],sum(expected.values()))
        self.assertAlmostEqual(layer["constituent_absorption_m_inv"]["keratin"][0]*4e-6/.65,
                               L*VALUES["keratin_abs"])
        self.assertAlmostEqual(layer["water_loading"]+layer["lipid_loading"]+.65,1.20)

    def test_viable_dna_mask_colloid_and_no_homogenized_organelle(self):
        result=self.compose()
        mel=100*math.log(10)*(VALUES["eumelanin_extinction"]*90+VALUES["pheomelanin_extinction"]*4)
        for i,layer in enumerate(result["layers"]):
            terms=layer["constituent_absorption_m_inv"]
            if 0 < i < 4:
                self.assertAlmostEqual(terms["dna"][0],math.log(10)*VALUES["dna_abs"]/.01*3.7)
                self.assertAlmostEqual(terms["colloidal_melanin"][0],.008*mel)
                self.assertAlmostEqual(layer["particles"]["interior_absorption_m_inv"][0],mel)
                self.assertNotIn("organelle",terms)
            else:
                self.assertNotIn("dna",terms)
                self.assertNotIn("colloidal_melanin",terms)
                self.assertIsNone(layer["particles"])

    def test_whole_blood_pool_conservation_basis_and_once_only_blood_loading(self):
        result=self.compose()
        c=result["blood_concentrations_kg_per_m3"]
        self.assertEqual(c["functional"],144)
        self.assertEqual(c["oxy"],108)
        self.assertEqual(c["deoxy"],36)
        self.assertEqual(c["oxy"]+c["deoxy"]+c["met"]+c["co"]+c["sulf"],147)
        terms=result["layers"][4]["constituent_absorption_m_inv"]
        factors={"oxy":("oxy_hemoglobin_extinction",108,64500),
                 "deoxy":("deoxy_hemoglobin_extinction",36,64500),
                 "met":("methemoglobin",1.5,66500),"co":("carboxyhemoglobin",1.5,66500),
                 "sulf":("sulfhemoglobin",0,66500),
                 "beta_carotene":("beta_carotene",.00007,537),
                 "bilirubin":("bilirubin",.003,585)}
        for name,(curve,concentration,mass) in factors.items():
            expected=.002*100*math.log(10)*VALUES[curve]*concentration/mass
            self.assertAlmostEqual(terms["blood_"+name][0],expected,
                                   msg=f"whole-blood {name} must have exactly one blood-volume factor")

    def test_oxygenation_endpoints_and_s2_source_default(self):
        self.assertAlmostEqual(self.compose("S2")["blood_concentrations_kg_per_m3"]["oxy"],136.8)
        for saturation,oxy,deoxy in [(0,0,144),(100,144,0)]:
            self.tables["table_iii_fields"]["oxygenated_blood_fraction_percent"]["values"]["S1"]=saturation
            c=self.compose()["blood_concentrations_kg_per_m3"]
            self.assertEqual((c["oxy"],c["deoxy"]),(oxy,deoxy))

    def test_water_anchor_difference_and_custom_pure_water(self):
        self.curves["palmer_williams_real_ior"]["values"]=[1.35,1.30,1.25]
        result=self.compose(waves=[250.,1300.,2500.])
        for layer in result["layers"]:
            anchor=layer["index_anchor_1300_nm"]
            self.assertEqual(layer["real_refractive_index"][1],anchor)
            self.assertAlmostEqual(layer["real_refractive_index"][0],anchor+layer["water_loading"]*.05)
            self.assertAlmostEqual(layer["real_refractive_index"][2],anchor-layer["water_loading"]*.05)
        self.tables["table_iv_fixed_source_values"]["water_content_percent"]["values"]["epidermis"]=100
        with self.assertRaisesRegex(ValueError,"pure-water"):
            self.compose()
        self.tables["table_iv_fixed_source_values"]["skin_layer_refractive_index_at_1300_nm"]["values"]["epidermis"]=1.30
        self.assertEqual(self.compose(waves=[250.])["layers"][1]["real_refractive_index"],[1.35])

    def test_source_fractions_axes_counts_and_layer_thickness(self):
        for preset,expected in [("S1",.01),("S2",.0375),("S3",.03),("S4",.10)]:
            result=self.compose(preset)
            self.assertEqual([x["layer_id"] for x in result["layers"]],LAYER_ORDER)
            self.assertEqual(result["layers"][3]["particles"]["organelle_fraction"],expected)
        result=self.compose("S3")
        self.assertAlmostEqual(result["layers"][0]["thickness_m"],4e-6,places=18)
        for i,q in [(1,.25),(2,.5),(3,1)]:
            particle=result["layers"][i]["particles"]
            self.assertAlmostEqual(particle["semi_major_m"],.41e-6*q/2)
            self.assertAlmostEqual(particle["semi_minor_m"],.17e-6*q/2)
            self.assertAlmostEqual(particle["complex_diameter_m"],.82e-6*q)
            self.assertEqual(particle["maximum_internal_count"],5)
        # S1 fractions are equal across layers, so the inverse size rate is exact.
        layers=self.compose()["layers"]
        self.assertAlmostEqual(layers[1]["particles"]["encounter_rate_m_inv"],4*layers[3]["particles"]["encounter_rate_m_inv"])

    def test_sphere_limit_rate_and_full_diameter_convention(self):
        self.tables["table_iii_fields"]["melanosome_dimensions_um"]["values"]["S4"]=[.4,.4]
        p=self.compose("S4")["layers"][3]["particles"]
        # Sphere radius is .2um, not .4um; geometric rate = 3v/(4r).
        self.assertAlmostEqual(p["encounter_rate_m_inv"],3*.1/(4*.2e-6))
        self.assertIsNone(p["complex_diameter_m"])

    def test_extreme_spheroid_limits_without_asin_cancellation(self):
        a=1e-7
        self.assertEqual(_spheroid_surface_volume(a,a),3/a)
        for r in (1e-8,1e-12,1e-100):
            # At r<=1e-8 the analytic expansion is pi/2*(1+r²/2)
            # through the first correction; omitted terms are below an ulp.
            expected=3*math.pi/(4*a)*(1+r*r/2)
            self.assertTrue(math.isclose(_spheroid_surface_volume(a,a/r),expected,rel_tol=5e-16),
                            f"slender prolate r={r}: must approach 3pi/(4a)")

    def test_positive_conversion_underflow_and_overflow_are_not_inactive_zero(self):
        self.assertEqual(_product("true zero",0,1e-300),0)
        for factors in [(1e-300,1e-300),(1e300,1e300)]:
            with self.assertRaisesRegex(ValueError,"underflow/overflow"):
                _product("synthetic product",*factors)
        # Nested tissue/blood loading must be checked before a zero-loading shortcut.
        self.tables["table_iii_fields"]["blood_content_percent_by_dermis_layer"]["values"]["S1"]=[1e-200,1e-200]
        self.tables["table_iv_fixed_source_values"]["whole_blood_bilirubin_concentration_mg_per_ml"]["value"]=1e-200
        with self.assertRaisesRegex(ValueError,"blood bilirubin loading.*underflow"):
            self.compose()
        self.tables=json.loads(PRESETS_PATH.read_text())
        self.tables["table_iv_fixed_source_values"]["beta_carotene_concentration_mg_per_ml"]["values"]["stratum_corneum"]=1e-200
        self.curves["beta_carotene"]["values"]=[1e-200]*3
        with self.assertRaisesRegex(ValueError,"beta_carotene coefficient.*underflow"):
            self.compose()

    def test_strict_interpolation_preserves_positive_subnormal_or_diagnoses_underflow(self):
        tiny=math.ulp(0.0)
        curve=self.curves["water_absorption"]
        curve["values"]=[tiny,tiny,tiny]
        self.assertEqual(StrictImportedCurves(self.curves).evaluate_with_provenance("water_absorption",775.)["value"],tiny)
        curve["values"]=[0,tiny,tiny]
        with self.assertRaisesRegex(ValueError,"water_absorption interpolation.*underflow"):
            StrictImportedCurves(self.curves).evaluate_with_provenance("water_absorption",775.)

    def test_atomic_unique_sibling_writes_and_cleanup(self):
        import os
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/"specimen.json"
            output.write_text("previous complete file")
            with patch("hylios_compose_specimen.os.replace",side_effect=OSError("test failure")):
                with self.assertRaisesRegex(OSError,"test failure"):
                    write_specimen({"first":1},output)
            self.assertEqual(output.read_text(),"previous complete file")
            self.assertEqual(list(Path(directory).iterdir()),[output])
            barrier=Barrier(2)
            original_replace=os.replace
            paths=[]
            def synchronized_replace(source,target):
                paths.append(Path(source))
                barrier.wait(timeout=5)
                original_replace(source,target)
            with patch("hylios_compose_specimen.os.replace",side_effect=synchronized_replace):
                with ThreadPoolExecutor(max_workers=2) as workers:
                    futures=[workers.submit(write_specimen,{"writer":i},output) for i in (1,2)]
                    for future in futures:
                        future.result()
            self.assertEqual(len(set(paths)),2,"simultaneous writers must use unique sibling temporaries")
            self.assertIn(json.loads(output.read_text())["writer"],(1,2))
            self.assertEqual(list(Path(directory).iterdir()),[output])

    def test_strict_missing_and_uncovered_only_active_curves(self):
        del self.curves["sulfhemoglobin"]
        self.compose()  # sulf is zero, so no spectrum is needed
        self.tables["table_iv_fixed_source_values"]["sulfhemoglobin_concentration_mg_per_ml"]["value"]=1
        with self.assertRaisesRegex(ValueError,"missing active curve sulfhemoglobin"):
            self.compose()
        self.tables["table_iv_fixed_source_values"]["sulfhemoglobin_concentration_mg_per_ml"]["value"]=0
        self.curves["dna_abs"]["wavelength_nm"]=[250.,300.,350.]
        with self.assertRaisesRegex(ValueError,"uncovered active curve dna_abs"):
            self.compose()
        self.tables["table_iv_fixed_source_values"]["skin_dna_density_mg_per_ml"]["value"]=0
        self.compose()

    def test_no_melanin_queries_when_both_colloid_and_particles_disabled(self):
        for key in ("melanosome_content_percent_by_epidermal_layer","colloidal_melanin_content_percent_by_epidermal_layer"):
            self.tables["table_iii_fields"][key]["values"]["S1"]=[0,0,0]
        del self.curves["eumelanin_extinction"],self.curves["pheomelanin_extinction"]
        result=self.compose()
        self.assertFalse(any("melanin_extinction" in x["curve_id"] for x in result["provenance"]["raw_evaluations"]))
        self.assertEqual(result["layers"][2]["particles"]["interior_absorption_m_inv"],[0,0])

    def test_zero_blood_needs_no_blood_only_spectra(self):
        self.tables["table_iii_fields"]["blood_content_percent_by_dermis_layer"]["values"]["S1"]=[0,0]
        for key in ("oxy_hemoglobin_extinction","deoxy_hemoglobin_extinction","carboxyhemoglobin","methemoglobin","sulfhemoglobin","bilirubin"):
            del self.curves[key]
        result=self.compose()
        self.assertTrue(all(v==[0,0] for k,v in result["layers"][4]["constituent_absorption_m_inv"].items() if k.startswith("blood_")))

    def test_invalid_inputs_fail_instead_of_clamping(self):
        for bad in [-1,float("nan"),float("inf"),True]:
            with self.subTest(bad=bad):
                tables=copy.deepcopy(self.tables)
                tables["table_iv_fixed_source_values"]["skin_dna_density_mg_per_ml"]["value"]=bad
                with self.assertRaises(ValueError):
                    compose_specimen(StrictImportedCurves(self.curves),"S1",[500.],presets=tables)
        self.tables["table_iv_fixed_source_values"]["methemoglobin_concentration_mg_per_ml"]["value"]=148
        with self.assertRaisesRegex(ValueError,"total Hb below"):
            self.compose()

    def test_fraction_axes_unit_and_transformed_quantity_rejections(self):
        tables=copy.deepcopy(self.tables)
        tables["table_iii_fields"]["melanosome_content_percent_by_epidermal_layer"]["values"]["S1"]=[101,0,0]
        with self.assertRaisesRegex(ValueError,"fraction"):
            compose_specimen(StrictImportedCurves(self.curves),"S1",[500.],presets=tables)
        tables=copy.deepcopy(self.tables)
        tables["table_iii_fields"]["melanosome_dimensions_um"]["values"]["S1"]=[.1,.2]
        with self.assertRaisesRegex(ValueError,"major diameter"):
            compose_specimen(StrictImportedCurves(self.curves),"S1",[500.],presets=tables)
        tables=copy.deepcopy(self.tables)
        tables["table_iv_fixed_source_values"]["skin_dna_density_mg_per_ml"]["unit"]="g/mL"
        with self.assertRaisesRegex(ValueError,"source unit mismatch"):
            compose_specimen(StrictImportedCurves(self.curves),"S1",[500.],presets=tables)
        self.curves["keratin_abs"]["source_quantity"]="Napierian_absorption_coefficient"
        with self.assertRaisesRegex(ValueError,"source basis mismatch"):
            StrictImportedCurves(self.curves)

    def test_content_identity_and_complete_evaluation_provenance(self):
        a=self.compose()
        self.assertEqual(a["composition_revision"],COMPOSITION_REVISION)
        self.assertEqual(len(a["resolved_specimen_sha256"]),64)
        self.assertFalse(a["provenance"]["spectral_bundle"]["approximate"])
        self.assertFalse(a["approximate"])
        self.assertEqual(a["coverage_profile_id"],"strict_source_coverage_v1")
        self.assertEqual(a["spectral_bundle_sha256"],a["provenance"]["spectral_bundle"]["bundle_sha256"])
        anchor=[x for x in a["provenance"]["raw_evaluations"] if x["curve_id"]=="palmer_williams_real_ior" and x["wavelength_nm"]==1300.]
        self.assertEqual(len(anchor),1)
        self.assertTrue(all(x["source_files"] and x["evaluation_trace"] for x in a["provenance"]["raw_evaluations"]))
        self.curves["water_absorption"]["values"][0]+=1
        b=self.compose()
        self.assertNotEqual(a["resolved_specimen_sha256"],b["resolved_specimen_sha256"])
        self.assertNotEqual(a["provenance"]["spectral_bundle"]["bundle_sha256"],b["provenance"]["spectral_bundle"]["bundle_sha256"])

    def test_actual_approximation_adapter_keeps_raw_basis_and_identity(self):
        from hylios_approximation_profile import ApproximationProfile,PROFILE_PATH,bundle_fingerprint
        profile=json.loads(PROFILE_PATH.read_text())
        curves=fixtures()
        for key,curve in curves.items():
            curve["provenance"]["source_files"]=profile["curves"][key]["source_files"]
        substitutions={}
        for key,spec in profile["substitution_sources"].items():
            substitutions[key]={"curve_id":key,"source_quantity":spec["source_quantity"],
                                "source_unit":spec["source_unit"],"wavelength_nm":[200.,400.],
                                "values":[1.35,1.34] if spec["source_quantity"]=="real_refractive_index" else [.01,.02],
                                "reference_basis":"synthetic fixture", "provenance":{"source_files":spec["source_files"]}}
        bundle={"schema_version":1,"approximate":True,"profile_id":profile["profile_id"],
                "profile":profile,"curves":curves,"substitution_curves":substitutions}
        bundle["bundle_sha256"]=bundle_fingerprint(bundle)
        result=compose_specimen(ApproximationProfile(bundle),"S1",[500.,1300.],presets=self.tables)
        self.assertTrue(result["provenance"]["spectral_bundle"]["approximate"])
        self.assertTrue(result["approximate"])
        self.assertEqual(result["coverage_profile_id"],profile["profile_id"])
        self.assertEqual(result["spectral_bundle_sha256"],bundle["bundle_sha256"])
        self.assertEqual(result["provenance"]["spectral_bundle"]["bundle_sha256"],bundle["bundle_sha256"])
        self.assertAlmostEqual(result["layers"][1]["constituent_absorption_m_inv"]["dna"][0],math.log(10)*VALUES["dna_abs"]/.01*3.7)


if __name__ == "__main__":
    unittest.main()
