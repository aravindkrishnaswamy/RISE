// Actual existing native context qualification; no owner numerical execution.
namespace RISE {bool FireProductionResidentContextResourceQualification(unsigned int mode);}
int main()
{
    if(!RISE::FireProductionResidentContextResourceQualification(0u))return 2;
    if(RISE::FireProductionResidentContextResourceQualification(1u))return 3;
    if(!RISE::FireProductionResidentContextResourceQualification(2u))return 4;
    if(!RISE::FireProductionResidentContextResourceQualification(3u))return 5;
    if(RISE::FireProductionResidentContextResourceQualification(4u))return 6;
    return 0;
}
